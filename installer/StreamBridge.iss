; Installer for Stream Bridge, made with Inno Setup 6 (https://jrsoftware.org/isinfo.php).
; CI builds it after the plugin:
;   ISCC /DAppVersion=0.3.0 installer\StreamBridge.iss
; It installs the plugin into the standard VST3 folder. If no virtual cable is found, it
; offers to open the VB-Cable download page. The uninstaller also removes the tools the
; plugin downloaded for the YouTube downloader.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

#ifndef PluginDir
  #define PluginDir "..\build\StreamBridge_artefacts\Release\VST3"
#endif

[Setup]
; Never change AppId: Windows uses it to recognise updates of the same program.
AppId={{C8AC063B-DBEE-4EB6-BA18-9AC27B88A967}
AppName=Stream Bridge
AppVersion={#AppVersion}
AppPublisher=Kursat
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\build\installer
OutputBaseFilename=Stream-Bridge-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
UninstallDisplayName=Stream Bridge

[Files]
Source: "{#PluginDir}\Stream Bridge.vst3\*"; DestDir: "{commoncf64}\VST3\Stream Bridge.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Stream Bridge.vst3"
; yt-dlp and QuickJS, downloaded by the plugin the first time the YouTube downloader is used.
Type: filesandordirs; Name: "{localappdata}\Stream Bridge"

[Run]
Filename: "https://vb-audio.com/Cable/"; Description: "No virtual cable was found. Open the free VB-Cable download page"; Flags: postinstall shellexec skipifsilent; Check: not HasVirtualCable

[Code]
// True if Windows has an output from a virtual audio driver, like VB-Audio's cables.
function HasVirtualCable: Boolean;
var
  Devices: TArrayOfString;
  I: Integer;
  SoundCard: String;
begin
  Result := False;

  if not RegGetSubkeyNames (HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render', Devices) then
    Exit;

  for I := 0 to GetArrayLength (Devices) - 1 do
    if RegQueryStringValue (HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\' + Devices[I] + '\Properties',
                            '{b3f8fa53-0004-438e-9003-51a46e139bfc},6', SoundCard) then
      if (Pos ('VB-Audio', SoundCard) > 0) or (Pos ('Virtual', SoundCard) > 0) then
      begin
        Result := True;
        Exit;
      end;
end;
