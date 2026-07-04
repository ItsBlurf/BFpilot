import os
def rewrite_clang(path):
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read()
    
    start_idx = text.find('for ARG in')
    end_idx = text.find('if [[ "${TU}" == "" ]]; then')
    if end_idx == -1: end_idx = text.find('if [ "${TU}" == "" ]; then')
    if end_idx == -1: end_idx = text.find('if [ "${TU}" = "" ]; then')
    
    if start_idx != -1 and end_idx != -1:
        new_loop = '''for ARG in "$@"; do
    case "$ARG" in
        -nostdlibc|-nodefaultlibs)
            LIBS_C=""
            LIBS_DEPS=""
            LIBS_KERN=""
            ;;
        -lkernel|-lkernel_sys|-lkernel_web)
            LIBS_KERN=""
            ;;
        -nostartfiles|-c|-shared)
            LIBS_CRT=""
            ;;
        -*)
            ;;
        *)
            TU="$ARG"
            ;;
    esac
done
'''
        text = text[:start_idx] + new_loop + text[end_idx:]
        text = text.replace('if [[ "${TU}" == "" ]]; then', 'if [ "${TU}" = "" ]; then')
        text = text.replace('if [ "${TU}" == "" ]; then', 'if [ "${TU}" = "" ]; then')
        text = text.replace('if [[ $($SCRIPT_DIR/prospero-llvm-config --version | cut -d. -f1) -ge 20 ]]; then', 'if [ $($SCRIPT_DIR/prospero-llvm-config --version | cut -d. -f1) -ge 20 ]; then')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        print('Fixed ' + path)

rewrite_clang('C:/Users/Blurf/ps5dev/toolchains/ps5-payload-sdk/bin/prospero-clang')
rewrite_clang('C:/Users/Blurf/ps5dev/toolchains/ps5-payload-sdk/bin/prospero-clang++')
