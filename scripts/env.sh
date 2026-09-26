if [[ -z "${ZSH_VERSION:-}" ]]; then
  print -u2 "scripts/env.sh must be sourced from zsh"
  return 1
fi

typeset _psx_env_script="${(%):-%N}"
typeset _psx_env_root="${_psx_env_script:A:h:h}"

export PSN00BSDK_HOME="${_psx_env_root}/.local/psn00bsdk"
export PSN00BSDK_LIBS="${PSN00BSDK_HOME}/lib/libpsn00b"
export PCSX_REDUX="${_psx_env_root}/.local/PCSX-Redux.app/Contents/MacOS/PCSX-Redux"
export PCSX_REDUX_BIOS="${_psx_env_root}/.local/PCSX-Redux.app/Contents/Resources/share/pcsx-redux/resources/openbios.bin"
export PCSX_REDUX_DATA="${_psx_env_root}/.local/pcsx-redux-data"
export PATH="${PSN00BSDK_HOME}/bin:${HOME}/.cargo/bin:/opt/homebrew/bin:${PATH}"

unset _psx_env_script _psx_env_root

