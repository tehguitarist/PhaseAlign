; Phase Align VST3 installer (NSIS). Windows has no AU format, so there's no plugin-type choice
; here — VST3 only, installed to the shared system VST3 folder.
;
; Build with (from repo root, after building PhaseAlign_VST3):
;   makensis /DVERSION=0.4.0 /DARTEFACTS_DIR=build\PhaseAlign_artefacts\Release\VST3 installer\windows\PhaseAlign.nsi
;
; ARTEFACTS_DIR should point at the directory CONTAINING "Phase Align.vst3" (i.e. the VST3 release
; output folder), not the bundle itself.

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef ARTEFACTS_DIR
  !define ARTEFACTS_DIR "..\..\build\PhaseAlign_artefacts\Release\VST3"
!endif

Name "Phase Align"
OutFile "PhaseAlign-Windows-v${VERSION}-Installer.exe"
InstallDir "$COMMONFILES64\VST3"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Phase Align VST3 Plugin" SecVST3
    SetOutPath "$INSTDIR\Phase Align.vst3"
    File /r "${ARTEFACTS_DIR}\Phase Align.vst3\*.*"

    WriteUninstaller "$INSTDIR\Phase Align.vst3\Uninstall-PhaseAlign.exe"

    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Phase Align" \
        "DisplayName" "Phase Align VST3"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Phase Align" \
        "UninstallString" "$INSTDIR\Phase Align.vst3\Uninstall-PhaseAlign.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Phase Align" \
        "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Phase Align" \
        "Publisher" "Leigh Pierce"
SectionEnd

Section "Uninstall"
    RMDir /r "$INSTDIR\Phase Align.vst3"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Phase Align"
SectionEnd
