# Messenger Project Failures

## Scope

This report covers the four prebuilt Messenger projects under
`/Users/bytedance/Downloads/tets`:

- `p2team01`
- `p2team03`
- `p2team05`
- `p2team07`

Each project was tested with the existing macOS player at
`build/mac-release/declgl-player`. The projects were not rebuilt.

## Summary

The projects do not all fail for the same reason:

| Project | First fatal error with the correct asset root | Other observed or confirmed blockers |
| --- | --- | --- |
| `p2team01` | Custom shader `level-selector-preview-glow` fails to compile | `slash-tint` and `damage-fog` have the same shader failure; one registered SVG and eight registered WebP textures cannot be decoded |
| `p2team03` | SVG texture `logo` cannot be decoded | `assets/audio/button.ogg` is an Ogg Theora video, not an audio stream |
| `p2team05` | Custom shader `grid` fails to compile | `LaserEffect` has the same shader failure; dynamic effect source binding remains broken after compilation is fixed |
| `p2team07` | Custom shader `vortex` fails to compile | `darken` has the same shader failure; the SVG logo is unsupported; two referenced Ogg files contain Opus, which the player cannot decode |

There is also a launch-configuration trap common to all four projects:
omitting `--asset-root` makes the player search under each project's
`build/assets` directory, although the assets are actually in the sibling
`assets` directory.

The Elm `Compiled in DEV mode` message is a performance warning, not a
functional failure.

## Correct Launch Command

Run from the `declgl-desktop` repository:

```sh
build/mac-release/declgl-player \
  --script /Users/bytedance/Downloads/tets/<project>/build/main.js \
  --asset-root /Users/bytedance/Downloads/tets/<project> \
  --app-name <project> \
  --frames 20
```

`--frames 20` only makes reproduction bounded and deterministic. It is not
required for normal interactive use.

### Why `--asset-root` is required

`src/player/main.cc` defaults the asset root to the parent directory of the
script. For a script at `<project>/build/main.js`, that default is
`<project>/build`. A resource named `assets/foo.png` is consequently resolved
as `<project>/build/assets/foo.png`, but these projects store it at
`<project>/assets/foo.png`.

This was reproduced with `p2team03`. Without `--asset-root`, the player emitted
many `Unable to open file` errors and terminated with:

```text
declgl-player: font 'pixel' failed:
fopen(/Users/bytedance/Downloads/tets/p2team03/build/assets/fonts/pixel.json)
```

This missing-file result is an invocation error, not a missing project asset
and not an image/audio codec failure.

## Root Causes

### 1. GLSL ES translation creates a name collision

**Classification:** player bug

**Affected projects:** `p2team01`, `p2team05`, and `p2team07`

The programs are authored against the `elm-regl` custom-effect contract.
`REGL.Program.makeEffectSimple` intentionally creates a sampler uniform named
`texture`, maps it from `DynamicValue "texture"`, and tells effect authors to
sample it using:

```glsl
uniform sampler2D texture;
vec4 color = texture2D(texture, vuv);
```

The player accepts these programs as GLSL ES 1.00. During translation to GLSL
330, `src/renderer/programs/glsl_es_translator.cc` blindly replaces the
identifier `texture2D` with `texture`. The translated expression is therefore:

```glsl
vec4 color = texture(texture, vuv);
```

The user uniform named `texture` shadows the GLSL 330 builtin function with the
same name. The compiler consequently reports:

```text
Invalid call of 'texture' (not a function or subroutine uniform)
```

The later `Use of undeclared identifier` messages are cascades from the failed
initializer; they are not separate shader defects.

Observed affected programs:

- `p2team01`: `level-selector-preview-glow`, `slash-tint`, `damage-fog`
- `p2team05`: `grid`, `LaserEffect`
- `p2team07`: `vortex`, `darken`

This is not evidence that the project shaders are invalid for their declared
GLSL ES 1.00 target. The player's compatibility translation conflicts with the
documented `elm-regl` effect API. The translator/runtime must preserve the
sampler mapping while avoiding the GLSL 330 identifier collision.

### 2. Dynamic effects do not bind their source texture

**Classification:** player bug, currently masked by the shader compilation bug

**Affected projects:** every project using an `elm-regl` dynamic effect;
confirmed relevant to the custom effects in `p2team01`, `p2team05`, and
`p2team07`

`RenderableWalker::apply_effect` correctly places the source framebuffer
texture in `BuiltinTextures.texture` before preparing the effect program.
However, `DynamicProgram::prepare` explicitly ignores its
`BuiltinTextures` argument.

At the same time, `makeEffectSimple` represents the source sampler as
`DynamicValue "texture"`. `DynamicProgram` treats that as an ordinary dynamic
field and only looks for it in the effect's supplied fields. It does not fall
back to `BuiltinTextures.texture`.

As a result, fixing only the GLSL translation is insufficient: the source
sampler still has no reliable binding. Depending on residual GL state, an
effect can render blank, sample the wrong texture, or appear nondeterministic.
The dynamic-program path needs to recognize the builtin effect texture while
preserving normal dynamic uniform behavior.

### 3. The image decoder does not support SVG or WebP

**Classification:** player format limitation

**Affected projects:** `p2team01`, `p2team03`, and `p2team07`

All image resources go through `stbi_load` in
`src/resources/image_decoder.cc`. The bundled `stb_image` supports formats
such as JPEG, PNG, TGA, BMP, PSD, GIF, HDR, PIC, and PNM, but it does not
support SVG or WebP.

The resulting message is misleading because it combines unsupported formats
with corrupt input:

```text
Image not of any known type, or corrupt
```

The affected project resources are:

- `p2team01`
  - `assets/img/lock.svg`
  - `assets/img/checkpoint_active.webp`
  - `assets/img/checkpoint_inactive.webp`
  - `assets/img/grid.webp`
  - `assets/img/octava.webp`
  - `assets/img/orbis.webp`
  - `assets/img/spike.webp`
  - `assets/img/squaro.webp`
  - `assets/img/trion.webp`
- `p2team03`
  - `assets/logo.svg`
- `p2team07`
  - `assets/imgs/logo.svg`

`p2team01/assets/teamlogo.webp` also exists, but it was not among the resource
paths requested by the inspected built application, so it is not counted as
an observed runtime failure.

For `p2team03`, the SVG failure is fatal:

```text
Elm runtime: texture 'logo' failed: decode_image_file
declgl-player: texture 'logo' failed: decode_image_file
```

The compatibility decision is either to add SVG/WebP decoding to the player or
to define a narrower native-player asset contract and convert these resources
to a supported raster format such as PNG.

### 4. `.ogg` handling assumes every Ogg file is Vorbis

**Classification:** one project asset defect and one player codec limitation

`src/audio/audio_decoder.cc` selects its decoder by filename extension. Every
`.ogg` file is passed to `stb_vorbis_decode_filename`, so only Ogg Vorbis is
supported. Ogg is a container and may contain other codecs.

#### `p2team03`: Theora file in the audio directory

`file` identifies:

```text
assets/audio/button.ogg: Ogg data, Theora video
```

This is not an audio stream and cannot be decoded by an audio player. It is a
project asset defect, likely an incorrectly exported or substituted file. The
player reports:

```text
audio load 'assets/audio/button.ogg' failed:
stb_vorbis_decode_filename failed
```

The other inspected `p2team03` Ogg audio files are Vorbis and are compatible
when the asset root is correct.

#### `p2team07`: valid Opus audio unsupported by the player

`file` identifies:

```text
assets/audio/background.ogg: Ogg data, Opus audio
assets/audio/button.ogg: Ogg data, Opus audio
```

These are valid audio resources, but `stb_vorbis` cannot decode Opus. This is a
player codec limitation rather than corrupt project data. The remaining
inspected `p2team07` Ogg files are Vorbis.

The fatal shader error can stop the run before all asynchronous audio failures
are surfaced, but these two files are referenced by the built application and
will remain blockers after the shader issue is fixed unless the player adds
Opus support or the assets are transcoded to Vorbis/WAV.

## Per-Project Diagnosis

### `p2team01`

The first terminal error is:

```text
Elm runtime: custom program 'level-selector-preview-glow' failed to compile
```

The same translation collision occurs in `slash-tint` and `damage-fog`.
Asynchronous loading also reports failures for one SVG and eight WebP
textures. Its inspected Ogg files are Vorbis, so no audio codec issue was
identified.

Fixing only `level-selector-preview-glow` will not make this project run: the
shared shader translator, effect source binding, and unsupported image formats
must all be addressed.

### `p2team03`

This project does not hit the custom-shader translator failure. With the
correct asset root, its fatal error is the unsupported SVG logo. It also logs
the invalid Theora `button.ogg`.

Without the correct asset root, the output is dominated by false missing-file
errors and terminates on the missing font under `build/assets`. Diagnose this
project only with the explicit project-level asset root.

### `p2team05`

The first terminal error is:

```text
Elm runtime: custom program 'grid' failed to compile
```

`LaserEffect` has the same GLSL name collision. The inspected image and audio
assets do not introduce the SVG/WebP/Opus limitations found in the other
projects. After shader compilation is repaired, dynamic effect source binding
must still be fixed.

### `p2team07`

The first terminal error is:

```text
Elm runtime: custom program 'vortex' failed to compile
```

`darken` has the same GLSL name collision. The SVG logo also fails `stbi_load`.
Two referenced audio files contain Opus and are incompatible with the current
Vorbis-only Ogg path. After compilation is repaired, the dynamic effect source
binding bug will also apply.

## Remediation Ownership and Order

1. **Invocation:** always pass the project directory as `--asset-root`.
2. **Player:** make GLSL ES translation safe when a user uniform is named
   `texture`; preserve the corresponding program-uniform mapping.
3. **Player:** bind `BuiltinTextures.texture` for dynamic effect programs.
4. **Player or project asset pipeline:** choose whether native compatibility
   includes SVG, WebP, and Ogg Opus.
   - If yes, add appropriate decoders.
   - If no, convert SVG/WebP to PNG and Opus to Vorbis/WAV before packaging,
     and fail validation with an explicit unsupported-format message.
5. **`p2team03` project:** replace `assets/audio/button.ogg` with a real audio
   file. Adding Theora support to the audio decoder would not make this asset
   valid audio.

The player fixes should be tested across all four projects because early fatal
errors currently mask downstream failures. In particular, shader compilation
success alone is not sufficient evidence that custom effects work; their
source texture must also be visibly verified.

## Evidence Locations

- Asset-root default: `src/player/main.cc`
- GLSL ES token translation:
  `src/renderer/programs/glsl_es_translator.cc`
- Dynamic uniform handling and ignored builtin textures:
  `src/renderer/programs/dynamic_program.cc`
- Effect source texture construction:
  `src/renderer/renderable_walker.cc`
- Image decode path: `src/resources/image_decoder.cc`
- Supported `stb_image` formats: `third_party/stb/stb_image.h`
- Vorbis-only Ogg path: `src/audio/audio_decoder.cc`
- `elm-regl` effect contract:
  `~/.elm/0.19.2/packages/linsyking/elm-regl/10.0.0/src/REGL/Program.elm`
