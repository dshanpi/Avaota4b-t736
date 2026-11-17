#!/usr/bin/env python3
"""Generate one bounded H.264 fixture and streaming software-reference hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New private build directory')
    args = parser.parse_args()
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    fixture = work/'h264-1920x1080-30fps-60frames-b2.h264'
    command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
               '-i', 'testsrc2=size=1920x1080:rate=30', '-frames:v', '60',
               '-an', '-c:v', 'libx264', '-profile:v', 'high', '-level:v', '4.0',
               '-pix_fmt', 'yuv420p', '-preset', 'veryfast', '-crf', '24', '-threads', '2',
               '-r', '30', '-fps_mode', 'cfr',
               '-x264-params', 'ref=2:bframes=2:b-adapt=0:keyint=60:min-keyint=60:scenecut=0:repeat-headers=1:open-gop=0:force-cfr=1',
               '-f', 'h264', str(fixture)]
    subprocess.run(command, check=True)
    probe_command = ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
                     '-show_entries', 'stream=codec_name,profile,level,width,height,coded_width,coded_height,has_b_frames,pix_fmt,r_frame_rate,avg_frame_rate,nb_read_frames',
                     '-of', 'json', str(fixture)]
    probe = json.loads(subprocess.check_output(probe_command, text=True))
    stream = probe['streams'][0]
    if stream['width'] != 1920 or stream['height'] != 1080 or int(stream['nb_read_frames']) != 60 or not stream['has_b_frames']:
        raise ValueError('Unexpected fixture geometry, count, or missing B frames')
    reference_command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-hwaccel', 'none',
                         '-c:v', 'h264', '-i', str(fixture), '-map', '0:v:0', '-an',
                         '-pix_fmt', 'nv21', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-']
    frame_bytes = 1920 * 1080 * 3 // 2
    whole = hashlib.sha256()
    frames = []
    with (work/'software-decode.log').open('wb') as log:
        process = subprocess.Popen(reference_command, stdout=subprocess.PIPE, stderr=log)
        while True:
            frame = process.stdout.read(frame_bytes)
            if not frame:
                break
            if len(frame) != frame_bytes:
                process.kill()
                process.wait()
                raise ValueError('Partial software-reference frame')
            whole.update(frame)
            frames.append(hashlib.sha256(frame).hexdigest())
        process.stdout.close()
        if process.wait() != 0 or len(frames) != 60:
            raise ValueError('Software reference failed or had wrong frame count')
    trace_command = ['ffmpeg', '-hide_banner', '-loglevel', 'info', '-i', str(fixture),
                     '-map', '0:v:0', '-c:v', 'copy', '-frames:v', '1', '-bsf:v', 'trace_headers', '-f', 'null', '-']
    trace = subprocess.run(trace_command, check=True, capture_output=True, text=True)
    fields = ['max_num_ref_frames', 'pic_width_in_mbs_minus1', 'pic_height_in_map_units_minus1',
              'frame_mbs_only_flag', 'frame_cropping_flag', 'frame_crop_left_offset',
              'frame_crop_right_offset', 'frame_crop_top_offset', 'frame_crop_bottom_offset',
              'max_num_reorder_frames', 'max_dec_frame_buffering', 'num_units_in_tick',
              'time_scale', 'fixed_frame_rate_flag']
    trace_lines = [line for line in trace.stderr.splitlines() if any(field in line for field in fields)]
    (work/'sps-geometry.log').write_text('\n'.join(trace_lines)+'\n', encoding='utf-8')
    geometry = {}
    for field in fields:
        values = re.findall(r'\b'+field+r'\s+.*?=\s*(\d+)\s*$', trace.stderr, re.MULTILINE)
        if values:
            if len(set(values)) != 1:
                raise ValueError('Conflicting SPS geometry')
            geometry[field] = int(values[0])
    if geometry.get('pic_height_in_map_units_minus1') != 67 or geometry.get('frame_crop_bottom_offset') != 4:
        raise ValueError('Unexpected SPS padding/crop; inspect before hardware use')
    if geometry.get('time_scale') != 60 or geometry.get('num_units_in_tick') != 1 or geometry.get('fixed_frame_rate_flag') != 1:
        raise ValueError('Expected fixed 30fps SPS timing')
    report = {'schema': 1, 'status': 'SOFTWARE_REFERENCE_VERIFIED_HARDWARE_PENDING',
              'fixture': {'name': fixture.name, 'bytes': fixture.stat().st_size,
                          'sha256': hashlib.sha256(fixture.read_bytes()).hexdigest()},
              'ffmpeg_version': subprocess.check_output(['ffmpeg','-version'], text=True).splitlines()[0],
              'generator_command': command, 'ffprobe_command': probe_command, 'ffprobe': probe,
              'reference_command': reference_command,
              'software_reference': {'format': 'NV21', 'width':1920, 'height':1080,
                                     'frame_bytes':frame_bytes, 'frames':len(frames),
                                     'bytes':frame_bytes*len(frames), 'sha256':whole.hexdigest(),
                                     'frame_sha256':frames, 'raw_reference_saved':False},
              'sps_geometry': geometry,
              'hardware_output_rule': 'Compare visible 1920x1080 NV21. If OMX reports coded 1920x1088 YV12, crop bottom 8 then convert to NV21; inspect negotiated caps and do not blindly crop visible 1080 output.',
              'dma_budget': {'coded_linear_frame_bytes':1920*1088*3//2,
                             'estimate':'9-12 decoded coded-size surfaces require about 27-36MiB, plus bitstream/motion-vector/internal buffers; reserve conservatively 64MiB for this one run. This is an estimate, not an allocation measurement.',
                             'current_root_report':'Approximately40MiB quarantined from prior runs; 128MiB total bring-up heap cap. Confirm current allocation/budget and no poison before the single run; no automatic retry.'},
              'limits':['One 2-second H264 High Level4.0 fixture, B frames, ref2; no sustained playback/encoding/browser acceptance.',
                        'Reference is streamed into per-frame and whole-stream SHA256 without a 186MiB raw file.']}
    (work/'reference.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'fixture':report['fixture'], 'reference_sha256':whole.hexdigest(),
                      'frames':len(frames), 'reference_json':str(work/'reference.json'), 'sps':geometry}))


if __name__ == '__main__':
    main()
