(* Native OCaml capture tool for the ml-regl protobuf wire protocol.

   This program builds a small but representative ml-regl scene, drives it
   through [Regl_runtime.Make] with a "file dump" host, and writes a binary
   capture file containing every BackendCommandBatch / AudioCommandBatch /
   Renderable that the scene produced. The C++ replay tool in
   [declgl-desktop/tools/replay/] reads this file and pushes each record through
   libdeclgl's C ABI to verify that the wire format is bit-faithful between the
   JS-verified OCaml frontend and the new native backend.

   File format (little-endian): magic "DGLCAP01" 8 bytes u32 record_count record
   := u8 kind (1=BE_CMD, 2=AU_CMD, 3=VIEW) u64 ts_ms u32 len u8[] payload [len
   bytes] *)
open Ml_regl_core

let magic = "DGLCAP01"
let kind_be_cmd = 1
let kind_au_cmd = 2
let kind_view = 3

(* Tiny LE writer over an out_channel. We pre-stage records into a buffer so the
   final file gets a correct record_count header. *)

module BE = struct
  let u8 buf v = Buffer.add_char buf (Char.chr (v land 0xff))

  let u32 buf v =
    u8 buf v;
    u8 buf (v lsr 8);
    u8 buf (v lsr 16);
    u8 buf (v lsr 24)

  let u64 buf v =
    let lo = Int64.to_int (Int64.logand v 0xffffffffL) in
    let hi = Int64.to_int (Int64.shift_right_logical v 32) in
    u32 buf lo;
    u32 buf hi
end

(* The capture host: stash bytes into the staging buffer with a timestamp, keyed
   by kind. Time source is provided externally so the driver loop can advance
   virtual time deterministically. *)

let now_ref = ref 0.0
let staging = Buffer.create (1 lsl 16)
let record_count = ref 0

let write_record kind payload =
  let ts_ms = Int64.of_float !now_ref in
  BE.u8 staging kind;
  BE.u64 staging ts_ms;
  let len = Bytes.length payload in
  BE.u32 staging len;
  Buffer.add_bytes staging payload;
  incr record_count

module CaptureHost = struct
  let ship_backend_cmd payload = write_record kind_be_cmd payload
  let ship_audio_cmd payload = write_record kind_au_cmd payload
end

module Runtime = Regl_runtime.Make (CaptureHost)

(* --------- a small native scene that exercises the wire protocol ---------

   We deliberately keep this scene compact: enough to cover all major
   BackendCommand kinds and a few Renderable shapes, but not so large that the
   capture file becomes unwieldy on first run. *)

let texture_name = "enemy"
let cropped_texture_name = "enemy-crop"
let texture_url = "/test/assets/enemy.png"
let audio_url = "/test/assets/test.ogg"
let custom_program_name = "customFlat"

let custom_program : Regl_program.regl_program =
  {
    frag =
      "precision mediump float;\n\
       uniform vec4 color;\n\
       void main(){gl_FragColor=color;}";
    vert =
      "precision mediump float;\n\
       attribute vec2 position;\n\
       void main(){gl_Position=vec4(position,0.,1.);}";
    attributes = Some [ ("position", DynamicValue "position") ];
    uniforms = Some [ ("color", DynamicValue "color") ];
    elements = None;
    primitive = None;
    count = Some (Regl_program.static_number 3.0);
  }

type sound_state = Loading | Loaded of Regl_audio.source | Failed

type model = {
  ts : float;
  texture_loaded : bool;
  crop_loaded : bool;
  custom_ready : bool;
  sound : sound_state;
}

let initial : model =
  {
    ts = 0.0;
    texture_loaded = false;
    crop_loaded = false;
    custom_ready = false;
    sound = Loading;
  }

let init () : model * Regl_proto.regl_output list =
  let start_cfg : Regl_proto.regl_start_config =
    {
      virt_width = 1920.;
      virt_height = 1080.;
      fbo_num = 5;
      builtin_programs = None;
    }
  in
  let texture_opts : Regl_proto.texture_options option =
    Some
      {
        mag = Some MagNearest;
        min = Some MinLinear;
        crop = Some ((0, 0), (32, 32));
      }
  in
  ( initial,
    [
      Regl_proto.start_regl start_cfg;
      Regl_proto.config_regl { time_interval = Millisecond 16.0 };
      Regl_proto.load_texture texture_name texture_url None;
      Regl_proto.load_texture cropped_texture_name texture_url texture_opts;
      Regl_proto.load_audio audio_url;
      Regl_proto.create_regl_program custom_program_name custom_program;
    ] )

let audio (m : model) : Regl_audio.audio =
  match m.sound with
  | Loaded src -> Regl_audio.audio src 0.0
  | _ -> Regl_audio.silence

let update (m : model) (input : Regl_proto.regl_input) :
    model * Regl_audio.audio * Regl_proto.regl_output list =
  let m' =
    match input with
    | Regl_proto.Event (Regl_proto.UpdateTick ts) -> { m with ts }
    | Regl_proto.Event _ -> m
    | Regl_proto.REGLRecvMsg msg -> (
        match msg with
        | Regl_proto.REGLTextureLoaded t when t.name = texture_name ->
            { m with texture_loaded = true }
        | Regl_proto.REGLTextureLoaded t when t.name = cropped_texture_name ->
            { m with crop_loaded = true }
        | Regl_proto.REGLProgramCreated name when name = custom_program_name ->
            { m with custom_ready = true }
        | _ -> m)
    | Regl_proto.AudioMsg msg -> (
        match msg with
        | Regl_proto.AudioLoadSuccess { audio_url = url; source }
          when url = audio_url ->
            { m with sound = Loaded source }
        | Regl_proto.AudioLoadFailed { audio_url = url; _ } when url = audio_url
          ->
            { m with sound = Failed }
        | _ -> m)
  in
  (m', audio m', [])

let view (m : model) : Regl_common.renderable =
  let geom =
    [
      Regl_builtin_programs.triangle (120., 120.) (260., 300.) (60., 320.)
        (Color.rgb 0.9 0.3 0.3);
      Regl_builtin_programs.rect (520., 120.) (180., 100.)
        (Color.rgb 0.2 0.5 0.9);
      Regl_builtin_programs.circle (1130., 180.) 55. (Color.rgb 0.8 0.2 0.8);
    ]
  in
  let texture_overlay =
    if m.texture_loaded then
      [
        Regl_builtin_programs.rect_texture (360., 220.) (220., 240.)
          texture_name;
      ]
    else []
  in
  let custom =
    if m.custom_ready then
      [
        Regl_common.atomic custom_program_name
          [
            Regl_common.nums "position" [ -0.85; -0.25; 0.0; 0.85; 0.85; -0.25 ];
            Regl_common.nums "color" [ 0.9; 0.25; 0.65; 1.0 ];
          ];
      ]
    else []
  in
  Regl_common.group [] (geom @ texture_overlay @ custom)

(* --------- driver: build runtime, fake some events, write file ---------- *)

let frame_count =
  match Sys.getenv_opt "CAPTURE_FRAMES" with
  | Some s -> ( try int_of_string s with _ -> 60)
  | None -> 60

let frame_dt = 16.0 (* ms per tick *)

let drive_one_frame (h : model Runtime.handle) (frame_idx : int) =
  now_ref := float_of_int frame_idx *. frame_dt;
  h.update !now_ref;
  match h.view () with
  | Some payload -> write_record kind_view payload
  | None -> ()

(* Hand-built BackendEvent payloads so we can simulate "all loads succeeded" and
   let the scene advance past its load gates. We round-trip through the
   protoc-generated decoder so the format is exactly what the C++ side will send
   back. *)

let make_texture_loaded_pb (name : string) : bytes =
  let ev : Regl_proto.Backend_pb.BackendEvent.t =
    `Texture_loaded { name; width = 64; height = 64 }
  in
  Regl_proto.Backend_pb.BackendEvent.to_proto ev
  |> Ocaml_protoc_plugin.Writer.contents |> Bytes.unsafe_of_string

let make_program_created_pb (name : string) : bytes =
  let ev : Regl_proto.Backend_pb.BackendEvent.t = `Program_created name in
  Regl_proto.Backend_pb.BackendEvent.to_proto ev
  |> Ocaml_protoc_plugin.Writer.contents |> Bytes.unsafe_of_string

let () =
  let out_path =
    if Array.length Sys.argv >= 2 then Sys.argv.(1) else "capture.bin"
  in
  let h = Runtime.create_app ~init ~update ~view in
  h.init ();

  (* simulate the backend's load-success replies after frame 0 *)
  now_ref := 1.0;
  h.recv_regl_cmd_pb (make_texture_loaded_pb texture_name);
  h.recv_regl_cmd_pb (make_texture_loaded_pb cropped_texture_name);
  h.recv_regl_cmd_pb (make_program_created_pb custom_program_name);

  for i = 0 to frame_count - 1 do
    drive_one_frame h i
  done;

  (* write file: header + record_count + staged bytes *)
  let oc = open_out_bin out_path in
  output_string oc magic;
  let hdr = Buffer.create 4 in
  BE.u32 hdr !record_count;
  output_string oc (Buffer.contents hdr);
  output_string oc (Buffer.contents staging);
  close_out oc;
  Printf.printf "wrote %s: %d records (%d bytes payload)\n" out_path
    !record_count (Buffer.length staging)
