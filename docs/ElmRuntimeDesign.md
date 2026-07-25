# Elm Runtime Design

## Status and Scope

This design is implemented by `src/elm_host` and `src/player`. The document
also records the compatibility boundary for future changes.

This document proposes an additive Elm runtime for `declgl-desktop`. The
existing ml-regl/OCaml integration and protobuf protocol remain supported and
unchanged.

The first implementation supports Elm applications that:

- are compiled to JavaScript with Elm 0.19;
- use the conventional elm-regl ports;
- render exclusively by sending elm-regl JavaScript objects through `setView`;
- use elm-regl built-in programs; and
- may use `Browser.element` and DOM event subscriptions, but do not need visible
  HTML or browser layout.

The following are intentionally out of scope for the first implementation:

- save-as-texture render nodes (`_c = 4`);
- custom shaders and `createGLProgram`;
- rendering HTML, CSS, SVG, or Canvas DOM nodes;
- browser layout, accessibility, navigation, or browser developer tools;
- network APIs such as `fetch`, `XMLHttpRequest`, and WebSocket; and
- arbitrary third-party JavaScript that expects a complete web browser.

Encountering an unsupported elm-regl command or render node is an application
error. The player must report the object path and unsupported feature and stop,
rather than silently omit it and produce incorrect output.

## Goals

1. Load an Elm-generated JavaScript file at application startup. The desktop
   binary must not embed application-generated JavaScript.
2. Run Elm in an embedded JavaScript engine while SDL, OpenGL, assets, and the
   frame loop remain native.
3. Preserve the current ml-regl behavior and OCaml linking model.
4. Reuse the existing native renderer and its protobuf render representation.
5. Simulate enough DOM structure and browser events for `Browser.element` and
   Elm virtual-dom to run, without displaying or rasterizing DOM content.
6. Keep all JavaScript execution on the SDL/OpenGL main thread.

## Existing Architecture

The current execution path is:

```text
OCaml application
    |
    | serialized protobuf commands, events, and views
    v
CamlLoopHooks -> Runtime -> Engine -> RenderableWalker -> OpenGL
```

`Runtime` owns the SDL event loop and frame pacing. `Engine` owns the window,
OpenGL resources, assets, audio, and backend command dispatch.
`RenderableWalker` consumes protobuf `Renderable` trees.

`LoopHooks` separates the host from the frame loop, but its methods currently
carry serialized protobuf values. Protobuf is therefore both the ml-regl wire
format and the renderer's native intermediate representation.

Elm-regl has a different host contract. It emits normal JavaScript objects
through ports:

| Port | Direction | Purpose |
| --- | --- | --- |
| `execREGLCmd` | Elm to host | Start, configure, and load resources |
| `setView` | Elm to host | Commit the latest render tree |
| `reglupdate` | Host to Elm | Deliver one frame timestamp |
| `recvREGLCmd` | Host to Elm | Deliver resource completion events |

## Proposed Topology

Add a standalone `declgl-player` executable and keep the OCaml bridge as it is:

```text
                              +-----------------------------+
OCaml application ----------> | CamlLoopHooks                |
                              +-----------------------------+
                                             |
                                             | protobuf
                                             v
Elm app.js -> QuickJS -> ElmHost -> ElmAdapter -> Runtime -> Engine -> OpenGL
                                             ^
                                             |
                                      protobuf internal IR
```

QuickJS is linked into `declgl-player`, not into the OCaml-facing
`libdeclgl.a`. This avoids increasing or destabilizing every ml-regl binary.
The player and the OCaml bridge share the existing runtime, engine, renderer,
resource, audio, and logging targets.

Use a pinned QuickJS or QuickJS-NG revision through the project's dependency
system. The selected revision must be identical on Linux, macOS, and Windows.

### Player Command Line

The initial interface should be explicit and suitable for packaging:

```text
declgl-player --script app.js [--module Main] [--flags flags.json]
              [--asset-root assets] [--app-name name]
```

- `--script` is read at runtime and evaluated as classic Elm-generated JS.
- `--module` resolves a dotted path below `globalThis.Elm` and defaults to
  `Main`.
- `--flags` is optional JSON passed to `Elm.<module>.init`.
- `--asset-root` defaults to the script's directory.
- `--app-name` controls the window/persistence application identity.

A later package manifest can wrap these arguments, but is not required for the
first working integration.

## ElmHost

`ElmHost` owns all Elm-specific state:

```text
ElmHost
  - JSRuntime
  - JSContext
  - application object and port handles
  - headless DOM tree
  - timer and requestAnimationFrame queues
  - latest converted render tree
  - queued native backend commands
  - queued native-to-Elm events
```

### Initialization

Initialization proceeds in this order:

1. Create one `JSRuntime` and one `JSContext` on the main thread.
2. Configure memory, stack, and execution limits.
3. Install the browser compatibility globals described below.
4. Evaluate the Elm-generated JavaScript file.
5. Resolve `globalThis.Elm.<module>.init`.
6. Create a synthetic root DOM element.
7. Call `init({ node: root, flags })`, omitting `flags` when none were given.
8. Validate and retain the returned application object.
9. Subscribe native callbacks to `execREGLCmd` and `setView`.
10. Resolve the optional inbound ports `reglupdate` and `recvREGLCmd`.
11. Pump due zero-delay timers and QuickJS jobs so Elm's initial commands are
    delivered after the subscriptions exist.
12. Drain the queued startup commands. A valid application must issue `start`.
13. Initialize the native window and enter `Runtime::run()`.

Native callbacks invoked by outgoing Elm ports only convert and enqueue data.
They must not enter `Runtime::run()` from inside a QuickJS call. This prevents a
nested SDL loop and avoids retaining transient QuickJS stack values across the
entire application lifetime.

### JavaScript Execution Rules

- All QuickJS API calls occur on the main thread.
- Native worker threads never call JavaScript directly.
- QuickJS pending jobs are drained at defined scheduler points.
- Each drain has a job-count or elapsed-work bound. Exceeding it is reported as
  a runaway microtask loop.
- A QuickJS interrupt handler enforces a configurable execution budget for a
  single callback. It is a fault boundary, not normal frame scheduling.
- Every caught JavaScript exception is logged with message, stack, script name,
  and current host operation.
- `JSValue` ownership is explicit: retained values are duplicated and released
  during orderly shutdown.

## Headless DOM

The DOM exists only to satisfy Elm's `Browser.element`, virtual-dom updates,
and event listener installation. It never creates a native widget, draws
pixels, performs layout, or participates in the OpenGL render tree.

All visible rendering comes from the object last sent through `setView`.

### Required Object Model

Implement a small JavaScript DOM shim, backed either by JavaScript objects or
QuickJS native classes. It needs:

- `window`, `document`, and `document.body`;
- `document.createElement`, `createElementNS`, `createTextNode`, and
  `createDocumentFragment`;
- `document.getElementById`;
- `Node.parentNode`, `childNodes`, `appendChild`, `insertBefore`,
  `removeChild`, and `replaceChild`;
- element `tagName`, `nodeType`, `textContent`, `style`, and common properties;
- `setAttribute`, `removeAttribute`, `setAttributeNS`, and
  `removeAttributeNS`;
- `addEventListener`, `removeEventListener`, and `dispatchEvent`;
- `replaceData` and `length` for text nodes; and
- a no-op focus model with one focused element.

Tree mutations and attributes must be real enough for Elm virtual-dom's diff
and patch code. Pixel dimensions, element positions, computed style, and layout
queries are not implemented. Unsupported layout methods should return stable
zero/default values or throw a clear unsupported-browser-API error, depending
on whether Elm runtime code requires the method during normal initialization.

The `<canvas id="elm-regl-canvas">` created by `REGL.toHtmlWith` is only a
synthetic event target. It has no graphics context. `getContext()` must not
expose WebGL or 2D rendering.

### Browser Scheduler Globals

Provide:

- `setTimeout` and `clearTimeout`;
- `requestAnimationFrame` and `cancelAnimationFrame`;
- `Date.now`;
- `performance.now` and `performance.timeOrigin`; and
- `console.log`, `warn`, and `error` routed to native logging.

Timers are stored in a native monotonic-time priority queue and executed only
on the main thread. `requestAnimationFrame` callbacks run once at the start of
the next native frame. This is needed because Elm's `Browser.element` batches
virtual-dom patches through animation frames, even though that DOM is not
displayed.

## Event Simulation

SDL events are converted into browser-shaped event objects and dispatched into
the synthetic DOM. This supports Elm `Html.Events`, `Browser.Events`, and raw
JSON event decoders without adding DOM rendering.

### Event Targets

- Mouse events target `#elm-regl-canvas` when it exists, otherwise the root
  element.
- Keyboard events target the focused element, defaulting to the canvas, and
  then bubble through its ancestors.
- Window-level listeners receive events registered through `window`.
- Document-level listeners receive events registered through `document`.

The dispatcher implements capture, target, and bubble phases sufficiently for
Elm's listener options. It honors `preventDefault`, `stopPropagation`,
`defaultPrevented`, `target`, `currentTarget`, and `eventPhase`.

### Mouse Event Shape

Mouse events include at least:

- `type`, `button`, `buttons`, and `detail`;
- `clientX`, `clientY`, `offsetX`, `offsetY`, `pageX`, and `pageY`;
- `movementX` and `movementY`;
- `ctrlKey`, `shiftKey`, `altKey`, and `metaKey`; and
- `timeStamp`.

Coordinates use the renderer's virtual canvas coordinates after the same
letterbox/pillarbox conversion used by the native runtime. Browser `button` is
zero-based, so SDL button 1 maps to browser button 0.

### Keyboard Event Shape

Keyboard events include at least:

- `type`, `key`, `code`, `repeat`, and `location`;
- `ctrlKey`, `shiftKey`, `altKey`, and `metaKey`; and
- `timeStamp`.

Maintain a documented SDL-to-browser key mapping. `code` should be
layout-independent where SDL supplies enough information, while `key` should
represent the current logical key. Unknown keys retain a stable SDL-derived
string rather than being dropped.

The first implementation decodes the existing protobuf `Event` in `ElmHost`
and augments it with stable browser defaults. A richer shared native event
structure remains future work for wheel, text input, and complete modifier
state.

## Elm-to-Native Adapter

Elm JavaScript objects are translated directly to native protobuf objects in
C++. The guest does not load protobuf JavaScript and does not serialize JSON on
every frame.

The adapter reads QuickJS properties with strict type checks and reports paths
such as:

```text
setView.c[3].color: expected an array of four numbers
```

### Backend Commands

| Elm object | Native command | Notes |
| --- | --- | --- |
| `_c: "start"` | `StartRegl` | Maps virtual size, FBO count, built-ins, and app name |
| `_c: "config"` | `ConfigRegl` | Initially maps frame interval only |
| `_c: "loadTexture"` | `LoadTexture` | Maps name, path, filters, and optional crop |
| `_c: "loadFont"` | `LoadFont` | Maps name, atlas path, and JSON path |

`createGLProgram` is unsupported in the first version and causes a clear
application error.

Commands are converted into a `BackendCommandBatch` and queued. Startup
commands are drained before entering the loop. Commands produced during a
frame are returned by `LoopHooks::pull_commands()` at the next safe runtime
dispatch point.

### Render Trees

| Elm node | Native renderable |
| --- | --- |
| `_c = 0` | `AtomicRenderable` using `_p` and remaining fields |
| `_c = 1`, `_n = "clear"` | Atomic program `clear` with color/depth fields |
| `_c = 2` | `GroupRenderable` with `c`, `e`, and optional `_sc` camera |
| `_c = 3` | `CompositeRenderable` with compositor `_p`, `r1`, and `r2` |

Object field values map to the existing protobuf `Value` variants: number,
string, boolean, numeric array, or string array. `null` from `setView` means no
render tree for that frame.

`_c = 4` is unsupported in the first version and causes a view-conversion
error. No persistent FBO texture behavior is emulated.

The converted protobuf tree is cached by `ElmHost`. `pull_view()` serializes or
returns the latest committed tree using the current runtime interface. A later
optimization may add a typed render entry point to avoid this final native
serialize/parse pair; it is not required for initial correctness.

### Backend Responses

Native `BackendEvent` values are decoded and sent through
`app.ports.recvREGLCmd.send(...)` using elm-regl's existing object shape:

```javascript
{ _c: "loadTexture", response: { texture: name, width, height } }
{ _c: "loadFont", response: { font: name } }
```

Program-created responses are not needed while custom programs are
unsupported. Load failures are logged as application errors because the
current elm-regl public response type has no failure variant. A future
elm-regl protocol revision can add explicit failure responses without changing
this runtime boundary.

## Frame Lifecycle

The required logical order for an Elm frame is:

```text
1. Run due timers and previous requestAnimationFrame callbacks.
2. Pump SDL events and dispatch synthetic DOM events.
3. Drain QuickJS jobs caused by those events.
4. Send reglupdate(timestamp) to Elm.
5. Drain QuickJS jobs and immediate timers caused by the update.
6. Apply queued backend commands at a safe native dispatch point.
7. Pull and render the latest setView tree.
8. Upload the bounded number of ready assets.
9. Swap the SDL window and perform frame pacing.
```

The runtime exposes pre-event and pre-view scheduling hooks. Their default
implementations are no-op, so the OCaml host keeps its existing behavior.

The timestamp sent to `reglupdate` follows existing elm-regl-js behavior:

```text
performance.timeOrigin + performance.now()
```

It is based on a monotonic clock plus a startup wall-clock anchor, preventing
wall-clock adjustments from making frame time move backward.

`setView` commits a replacement view, not a queue. If Elm sends multiple views
before rendering, only the latest is rendered. If no new view arrives, the
last valid view remains active unless Elm explicitly sends `null`.

## Assets and Paths

The current loader resolves assets relative to the executable. For a player
that loads an external project, assets must instead resolve relative to an
explicit application asset root.

Refactor `AssetLoader` to receive an asset root at construction. The Elm player
uses `--asset-root` or the script directory. The OCaml path retains its current
executable-directory default.

Existing path safety rules remain mandatory:

- reject absolute asset paths;
- canonicalize the root and requested path;
- reject `..` traversal and symlink resolution outside the root; and
- perform file decoding on the existing worker thread.

HTTP and data URLs are unsupported initially. Their rejection should mention
that the desktop runtime accepts package-relative files only.

## Lifecycle and Shutdown

Shutdown order is:

1. Stop accepting new Elm callbacks.
2. Cancel timers and requestAnimationFrame callbacks.
3. Release retained application and DOM `JSValue` handles.
4. Drain or discard pending QuickJS jobs.
5. Destroy `JSContext` and `JSRuntime`.
6. Stop asset/audio workers and destroy OpenGL resources through the existing
   engine shutdown path.
7. Destroy the SDL window and quit SDL.

No worker may retain or access a QuickJS value. Resource completion records are
plain native data until delivered on the main thread.

## Error Policy

Fatal startup errors include:

- unreadable JavaScript or flags files;
- JavaScript parse/evaluation failure;
- missing Elm module or `init` function;
- invalid flags;
- missing required elm-regl ports;
- no `start` command during startup; and
- native window/OpenGL initialization failure.

Fatal runtime application errors include malformed command/view objects,
unsupported save-as-texture or custom-program commands, uncaught JavaScript
exceptions, and execution-limit interruption.

Asset I/O/decode failures are reported with the logical asset name, resolved
relative path, and native reason. Until elm-regl gains failure response values,
the player stops instead of leaving Elm indefinitely waiting for a success
event.

## Build Layout

Add these first-party targets:

```text
src/js_runtime/       QuickJS ownership, scheduler, and diagnostics
src/headless_dom/     DOM tree and synthetic browser events
src/elm_host/         Elm module/port lifecycle and object adapters
src/player/           declgl-player CLI and process entry point
```

Suggested CMake options:

```text
BUILD_OCAML_BRIDGE=ON
BUILD_ELM_PLAYER=ON
```

`BUILD_ELM_PLAYER=OFF` must avoid compiling or linking QuickJS. Existing
`libdeclgl.a` output and OCaml symbols remain unchanged.

## Implementation Plan

### Phase 1: JavaScript Host

- Add the pinned QuickJS dependency and `declgl-player` target.
- Implement script loading, module resolution, flags, console, diagnostics,
  memory limits, and interrupt handling.
- Implement timers, requestAnimationFrame, and QuickJS job draining.
- Prove that a small `Platform.worker` Elm program initializes and exchanges
  ports.

### Phase 2: Elm-Regl Adapter

- Bind the four elm-regl ports.
- Convert start/configure/texture/font commands.
- Convert atomic/group/composite/clear render trees.
- Convert texture/font completion events back to Elm values.
- Reject custom programs and save-as-texture with tested diagnostics.

### Phase 3: Headless Browser Runtime

- Implement the synthetic DOM tree and virtual-dom mutation APIs.
- Initialize existing `Browser.element` output against a synthetic root.
- Implement requestAnimationFrame-driven DOM patching.
- Verify that no DOM node can produce visible pixels or a GL context.

### Phase 4: SDL Event Simulation

- Introduce the shared native input-event representation.
- Dispatch mouse and keyboard events through the synthetic DOM.
- Implement event propagation, modifier state, focus, and coordinate mapping.
- Verify `Html.Events` and `Browser.Events` subscriptions.

### Phase 5: Packaging and Hardening

- Add explicit asset-root configuration while preserving OCaml defaults.
- Validate shutdown, reload failure paths, and resource completion ordering.
- Add release packaging for Linux, macOS, and Windows.
- Document the supported browser and elm-regl subset.

## Verification

### Unit Tests

- QuickJS value-to-protobuf conversion for every supported command and node.
- Path-aware diagnostics for malformed and unsupported values.
- Native backend-event-to-Elm object conversion.
- Timer ordering, cancellation, requestAnimationFrame, and microtask draining.
- DOM mutation and event propagation semantics.
- SDL-to-browser mouse/key mapping and virtual coordinate conversion.
- Asset-root containment and traversal rejection.

### Integration Tests

Compile and run desktop variants of elm-regl examples that only use built-in
programs:

- Basic;
- Camera;
- Mask, after replacing any custom program usage;
- Text;
- Stress; and
- relevant BugFixes cases without custom shaders.

Tests should cover both `Platform.worker` and `Browser.element`. The latter must
demonstrate that its virtual DOM updates and event handlers run while the only
visible output is the native declgl OpenGL window.

Use deterministic frame capture or framebuffer readback to compare known
scenes. Add long-running tests for QuickJS memory growth, repeated Elm updates,
asset completion during rendering, window resizing, and clean shutdown.

## Future Work

The following can be designed separately after the built-in Elm runtime is
stable:

- save-as-texture and persistent render-target ownership;
- custom shader support and GLSL ES-to-OpenGL translation;
- audio ports for Elm;
- richer gamepad, text-input, wheel, and touch events;
- a typed runtime/engine interface that removes the native protobuf
  serialize/parse step for Elm; and
- a project manifest and single-directory application bundle.
