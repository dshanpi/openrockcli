Unicode true
Target amd64-unicode
!include "MUI2.nsh"

!ifndef PAYLOAD
  !error "Define PAYLOAD with the staged package directory"
!endif
!ifndef OUTPUT
  !error "Define OUTPUT with the installer filename"
!endif

!define APP_NAME "OpenRockCLI Native Preview"
!define APP_VERSION "1.2.1-native-preview.1"
!define APP_KEY "Software\OpenRockCLI\NativePreview"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenRockCLI-NativePreview"

Name "${APP_NAME} (x64)"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\OpenRockCLI-Native-Preview"
InstallDirRegKey HKCU "${APP_KEY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
VIProductVersion "1.2.1.1"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "FileVersion" "1.2.1.1"
VIAddVersionKey "FileDescription" "OpenRockCLI native Windows development preview installer"
VIAddVersionKey "LegalCopyright" "OpenRockCLI contributors; see LICENSE"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PAYLOAD}/LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "$(FinishText)"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

LangString WelcomeText ${LANG_SIMPCHINESE} "安装 OpenRockCLI Windows x64 原生 Rockusb 开发测试版。$\r$\n$\r$\n支持厂家 rockusb.sys 驱动，无需替换为 WinUSB。请保留已安装的 Rockusb 驱动；本安装包不包含驱动。$\r$\n$\r$\n离线测试已通过，真实开发板烧录仍待验证。"
LangString WelcomeText ${LANG_ENGLISH} "Install the OpenRockCLI native Windows x64 development preview.$\r$\n$\r$\nKeep the manufacturer's rockusb.sys driver. No WinUSB replacement is required. The driver is not included.$\r$\n$\r$\nOffline tests have passed; hardware qualification is pending."
LangString FinishText ${LANG_SIMPCHINESE} "安装完成。$\r$\n$\r$\n打开桌面或开始菜单中的 OpenRockCLI Native Preview，输入 openrockcli scan 扫描设备。$\r$\n$\r$\n可从 Windows 应用列表卸载。"
LangString FinishText ${LANG_ENGLISH} "Installation complete.$\r$\n$\r$\nOpen OpenRockCLI Native Preview from the desktop or Start menu, then enter openrockcli scan.$\r$\n$\r$\nUninstall from Windows Installed apps."

Function .onInit
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "OpenRockCLI" Main
  SetOutPath "$INSTDIR"
  File "${PAYLOAD}/openrockcli.exe"
  File "${PAYLOAD}/OpenRockCLI.cmd"
  File "${PAYLOAD}/README.md"
  File "${PAYLOAD}/LICENSE"
  File "${PAYLOAD}/使用说明.txt"
  File "${PAYLOAD}/BUILD-INFO.txt"
  SetOutPath "$INSTDIR\docs"
  File "${PAYLOAD}/docs/WINDOWS_ROCKUSB.md"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$SYSDIR\cmd.exe" '/D /K $\"$\"$INSTDIR\OpenRockCLI.cmd$\"$\"'
  CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$SYSDIR\cmd.exe" '/D /K $\"$\"$INSTDIR\OpenRockCLI.cmd$\"$\"'
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\使用说明.lnk" "$INSTDIR\使用说明.txt"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME} (x64)"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "OpenRockCLI"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "Uninstall"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\使用说明.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\Uninstall.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  Delete "$INSTDIR\openrockcli.exe"
  Delete "$INSTDIR\OpenRockCLI.cmd"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\使用说明.txt"
  Delete "$INSTDIR\BUILD-INFO.txt"
  Delete "$INSTDIR\docs\WINDOWS_ROCKUSB.md"
  RMDir "$INSTDIR\docs"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"
SectionEnd
