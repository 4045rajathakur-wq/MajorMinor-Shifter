[Setup]
AppName=Major Minor Switcher
AppVersion=1.0.0
AppPublisher=MyStudio
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=Output
OutputBaseFilename=MajorMinorSwitcher-Setup
Compression=lzma
SolidCompression=yes
Uninstallable=yes

[Files]
Source: "build\MajorMinorSwitcher_artefacts\Release\VST3\Major Minor Switcher.vst3\*"; DestDir: "{commoncf64}\VST3\Major Minor Switcher.vst3"; Flags: recursesubdirs ignoreversion
