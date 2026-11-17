#!/usr/bin/env python3
"""Build a separate diagnostic Cedar demo without changing Tina or the WC bundle."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Patch anchor is not unique: ' + old[:80])
    return text.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--tina', type=Path, default=Path('/home/ubuntu/t736-aiot/tina'))
    parser.add_argument('--bridge', type=Path, default=Path('/home/ubuntu/t736-mainline/build/cedar-bridge-package-v2'))
    parser.add_argument('--parser-source', type=Path, help='Separately reviewed private EOF-fixed parser source')
    args = parser.parse_args()
    work, tina, bridge = args.work.resolve(), args.tina.resolve(), args.bridge.resolve()
    if work.exists() or work == tina or tina in work.parents:
        parser.error('work must be new and outside Tina')
    work.mkdir(parents=True)
    original = tina/'platform/allwinner/multimedia/libcedarc_v2.0/demo/vdecoderDemo'
    for name in ('vdecoderDemo.c', 'rawStreamParser.c', 'rawStreamParser.h'):
        shutil.copy2(original/name, work/name)
    if args.parser_source:
        shutil.copy2(args.parser_source, work/'rawStreamParser.c')
    source = work/'vdecoderDemo.c'
    text = source.read_text(encoding='utf-8', errors='surrogateescape')
    text = replace_once(text, '''    DecDemo *Decoder = (DecDemo *)param;
    void *nRet = NULL;  // MUST be void* for pthread_join (writes 8 bytes on 64-bit)''',
                        '''    DecDemo *Decoder = (DecDemo *)param;
    int nRet = 0; /* initDecoder status; thread return values are unused. */''')
    text = replace_once(text, '    int nEndOfStream=0;',
                        '    int nEndOfStream=0;\n    int last_result=-999; unsigned long diagnostic_loops=0;')
    text = replace_once(text, '            long long tDecEnd = GetNowUs();', r'''
            long long tDecEnd = GetNowUs();
            diagnostic_loops++;
            if (nRet != last_result || nRet == VDECODE_RESULT_FRAME_DECODED ||
                nRet == VDECODE_RESULT_KEYFRAME_DECODED || diagnostic_loops % 1000 == 0) {
                fprintf(stderr, "T736_DEMO: decode loop=%lu ret=%d eos=%d us=%lld sbm_frames=%d sbm_bytes=%d fbm_total=%d fbm_empty=%d fbm_valid=%d\n",
                    diagnostic_loops, nRet, nEndOfStream, tDecEnd-tDecStart,
                    VideoStreamFrameNum(pVideoDec,0), VideoStreamDataSize(pVideoDec,0),
                    TotalPictureBufferNum(pVideoDec,0), EmptyPictureBufferNum(pVideoDec,0),
                    ValidPictureNum(pVideoDec,0));
                last_result = nRet;
            }''')
    text = replace_once(text, '            if(nDispFrameCount >= pVideoDecoder->nFinishNum)', r'''
            fprintf(stderr, "T736_DEMO: saved_or_consumed frame=%d size=%dx%d format=%d; return_immediate\n",
                    nDispFrameCount, pPicture->nWidth, pPicture->nHeight, pPicture->ePixelFormat);
            if (ReturnPicture(pVideoDec, pPicture) != 0) {
                fprintf(stderr, "T736_DEMO: ReturnPicture failed\n");
                abort();
            }
            if(nDispFrameCount >= pVideoDecoder->nFinishNum)''')
    text = replace_once(text, '            addPictureToList(pVideoDec, pDisList, &DisHeader, &DisRear, pPicture);',
                        '            /* Picture already returned after saving; no simulated display queue. */')
    for thread in ('tparser', 'tdecoder', 'tdisplay', 'dram[i]'):
        text = replace_once(text, f'pthread_join({thread}, &nRet);', f'pthread_join({thread}, NULL);')
    text = replace_once(text, '    pDec->thread.nEndofStream = 1;',
                        '    fprintf(stderr, "T736_DEMO: parser reached EOF; publishing eos\\n");\n    pDec->thread.nEndofStream = 1;')
    source.write_text(text, encoding='utf-8', errors='surrogateescape')
    sdk = tina/'out/t736/demo_aiot/buildroot/buildroot/host'
    lib = bridge/'cedar/usr/lib/aarch64-linux-gnu'
    executable = work/'vdecoderdemo-diagnostic'
    command = [str(sdk/'bin/aarch64-none-linux-gnu-gcc'), '-O2', '-g',
               '-I'+str(bridge/'cedar/usr/include'),
               '-I'+str(original.parent.parent/'base/include'), '-I'+str(work),
               str(source), str(work/'rawStreamParser.c'), '-o', str(executable),
               '-L'+str(lib), '-Wl,-rpath-link,'+str(lib), '-lcdc_base', '-lMemAdapter',
               '-lvdecoder', '-lfbm', '-lsbm', '-lpthread', '-lm', '-lrt', '-ldl', '-lVE', '-lvideoengine']
    result = subprocess.run(command, capture_output=True, text=True)
    (work/'compile.log').write_text(result.stdout+result.stderr, encoding='utf-8')
    result.check_returncode()
    report = {'schema': 1, 'status': 'COMPILED_HARDWARE_PENDING',
              'original_sources': {name: sha(original/name) for name in ('vdecoderDemo.c','rawStreamParser.c','rawStreamParser.h')},
              'modified_sources': {name: sha(work/name) for name in ('vdecoderDemo.c','rawStreamParser.c','rawStreamParser.h')},
              'executable': str(executable), 'executable_sha256': sha(executable),
              'executable_bytes': executable.stat().st_size, 'command': command,
              'changes': ['Immediate ReturnPicture after saving, with return-code check.',
                          'Decode return transitions, periodic progress, SBM/FBM counts, EOF and saved frame diagnostics.',
                          'Separate integer initialization status from unused pthread_join return values; remove vendor int-to-pointer warning.',
                          'EOF-fixed Annex-B parser' if args.parser_source else 'Unmodified original parser'],
              'parser_source': str(args.parser_source) if args.parser_source else None,
              'limits': ['Parser validated offline only; hardware completion remains pending.' if args.parser_source else 'Raw parser EOF bug remains: last NAL is omitted.',
                         'No modification to original WC bundle or Tina source.',
                         'Run through the private WC wrapper; this executable is a separate private artifact.']}
    (work/'manifest.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
