; Peggy Pro 4.63 UTF-8 スロットパッチ インストーラー (Inno Setup 6)
;   ビルド:  ISCC.exe peggy_utf8.iss            -> ..\dist\PeggyUtf8Setup.exe
;   検証用:  ISCC.exe /DTESTBUILD peggy_utf8.iss -> 管理者権限なし・別名で出力 (本体には使わない)

#define AppName "Peggy Pro 4.63 UTF-8 パッチ"
#define AppVer  "1.0.0"
#define ExeSize 2383872

[Setup]
AppId={{B6F1C1A2-7C0E-4E55-9B1B-5A1D0A8E6B01}
AppName={#AppName}
AppVersion={#AppVer}
AppPublisher=非公式パッチ
DefaultDirName={autopf32}\Anchor\Peggy
AppendDefaultDirName=no
DirExistsWarning=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
CloseApplicationsFilter=peggypro.exe
RestartApplications=no
UninstallDisplayName={#AppName}
#ifdef TESTBUILD
PrivilegesRequired=lowest
OutputDir=..\build
OutputBaseFilename=PeggyUtf8Setup_test
#else
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename=PeggyUtf8Setup
#endif

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Files]
; peggypro.exe は元の exe をバックアップしてから置き換える。アンインストール時は Code 側でバックアップから復元する。
Source: "..\dist\peggypro.exe"; DestDir: "{app}"; Flags: ignoreversion uninsneveruninstall; BeforeInstall: BackupOriginalExe
Source: "..\dist\utf8slot.dll"; DestDir: "{app}"; Flags: ignoreversion

[Code]
const
  ExpectedExeSize = {#ExeSize};

function BackupPath: String;
begin
  Result := ExpandConstant('{app}\peggypro.exe.pre-utf8slot');
end;

procedure BackupOriginalExe;
var
  Exe: String;
begin
  Exe := ExpandConstant('{app}\peggypro.exe');
  if FileExists(Exe) and (not FileExists(BackupPath)) then
    FileCopy(Exe, BackupPath, False);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Exe: String;
  Sz: Integer;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    Exe := AddBackslash(WizardDirValue) + 'peggypro.exe';
    if not FileExists(Exe) then
    begin
      MsgBox('選択したフォルダに peggypro.exe が見つかりません。' + #13#10 +
             'Peggy Pro 4.63 のインストール先を指定してください。', mbError, MB_OK);
      Result := False;
    end
    else if FileSize(Exe, Sz) and (Sz <> ExpectedExeSize) then
      Result := MsgBox('peggypro.exe のサイズが Peggy Pro 4.63 と異なります。' + #13#10 +
                       '他のバージョンでは正しく動作しない可能性があります。続行しますか?',
                       mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Exe, Bak: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Exe := ExpandConstant('{app}\peggypro.exe');
    Bak := Exe + '.pre-utf8slot';
    if FileExists(Bak) then
    begin
      if FileCopy(Bak, Exe, False) then
        DeleteFile(Bak)
      else
        MsgBox('元の peggypro.exe を復元できませんでした。Peggy が起動していないか確認し、' + #13#10 +
               Bak + ' を peggypro.exe にコピーしてください。', mbError, MB_OK);
    end;
  end;
end;
