; Script generated by the Inno Setup Script Wizard.
; SEE THE DOCUMENTATION FOR DETAILS ON CREATING INNO SETUP SCRIPT FILES!

[Setup]
AppName=Celestia
AppVerName=Celestia 1.2.6
AppPublisher=Shatters Software
AppPublisherURL=http://www.shatters.net/celestia/
AppSupportURL=http://www.shatters.net/celestia/
AppUpdatesURL=http://www.shatters.net/celestia/
DefaultDirName={pf}\Celestia
DefaultGroupName=Celestia
LicenseFile=C:\celestia\celestia\COPYING
; uncomment the following line if you want your installation to run on NT 3.51 too.
; MinVersion=4,3.51

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; MinVersion: 4,4
Name: "urlassoc"; Description: "Associate cel:// &URLs"; GroupDescription: "Other tasks:"

[Dirs]
Name: "{app}\extras"
Name: "{app}\textures"
Name: "{app}\textures\hires"
Name: "{app}\textures\medres"
Name: "{app}\textures\lores"

[Files]
Source: "celestia.exe"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "start.cel"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "celestia.cfg"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "demo.cel"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "guide.cel"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "libpng1.dll"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "zlib.dll"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "README.txt"; DestDir: "{app}"; CopyMode: alwaysoverwrite; Flags: isreadme
Source: "controls.txt"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "ChangeLog"; DestDir: "{app}"; CopyMode: alwaysoverwrite
Source: "COPYING"; DestDir: "{app}"; CopyMode: alwaysoverwrite

; Data
Source: "data\stars.dat"; DestDir: "{app}/data"; CopyMode: alwaysoverwrite
Source: "data\solarsys.ssc"; DestDir: "{app}/data"; CopyMode: alwaysoverwrite
Source: "data\extrasolar.ssc"; DestDir: "{app}/data"; CopyMode: alwaysoverwrite
Source: "data\starnames.dat"; DestDir: "{app}/data"; CopyMode: alwaysoverwrite
Source: "data\galaxies.dat"; DestDir: "{app}/data"; CopyMode: alwaysoverwrite
Source: "data\asterisms.dat"; DestDir: "{app}/data"; CopyMode: alwaysoverwrite
Source: "data\boundaries.dat"; DestDir: "{app}/data"; CopyMode: alwaysoverwrite
Source: "data\galileo.xyz"; DestDir: "{app}/data"; CopyMode: alwaysoverwrite

; Textures
Source: "textures\flare.jpg"; DestDir: "{app}/textures"; CopyMode: alwaysoverwrite
Source: "textures\logo.png"; DestDir: "{app}/textures"; CopyMode: alwaysoverwrite

; Medium res textures
Source: "textures\medres\astar.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\bstar.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\gstar.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\mstar.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\ariel.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\asteroid.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\browndwarf.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\callisto.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\deimos.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\dione.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\earth.png"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\earth-clouds.png"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\earthnight.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\europa.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\ganymede.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\gasgiant.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\gaspramosaic.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\iapetus.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\idamosaic.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\io.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\iss-cap1.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\iss-mc1.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\iss-mc31.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\iss-sol1.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\jupiter.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\jupiterlike.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\mars.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\marsbump1k.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\mercury.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\mercurybump.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\mimas.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\miranda.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\moon.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\moonbump1k.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\neptune.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\oberon.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\phobos.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\pluto.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\plutobump1k.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\rhea.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\saturn.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\tethys.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\titania.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\triton.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\umbriel.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\venus.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite
Source: "textures\medres\venuslike.jpg"; DestDir: "{app}/textures/medres"; CopyMode: alwaysoverwrite

; Low res textures
Source: "textures\lores\astar.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\bstar.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\gstar.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\mstar.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\ariel.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\asteroid.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\browndwarf.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\callisto.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\charon.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\deimos.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\dione.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\earth.png"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\earth-clouds.png"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\earthnight.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\europa.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\ganymede.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\gasgiant.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\gaspramosaic.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\iapetus.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\idamosaic.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\io.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\jupiter.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\jupiterlike.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\mars.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\marsbump1k.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\mercury.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\mercurybump.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\mimas.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\miranda.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\moon.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\moonbump1k.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\neptune.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\oberon.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\phobos.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\pluto.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\plutobump1k.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\rhea.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\saturn.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\saturn-rings.png"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\tethys.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\titania.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\titan.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\triton.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\umbriel.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\uranus.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\uranus-rings.png"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\venus.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite
Source: "textures\lores\venuslike.jpg"; DestDir: "{app}/textures/lores"; CopyMode: alwaysoverwrite

; shaders
Source: "shaders\bumpdiffuse.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\bumphaze.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\diffuse.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\diffuse_texoff.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\haze.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\shadowtex.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\simple.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\specular.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\rings.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\ringshadow.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite
Source: "shaders\night.vp"; DestDir: "{app}/shaders"; CopyMode: alwaysoverwrite

; models
Source: "models\amalthea.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\bacchus.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\castalia.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\deimos.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\eros.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\galileo.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\gaspra.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\geographos.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\golevka.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\hubble.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\hyperion.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\ida.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\iss.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\kleopatra.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\ky26.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\mir.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\phobos.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\proteus.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\toutatis.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\vesta.3ds"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\asteroid.cms"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\borrelly.cms"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite
Source: "models\roughsphere.cms"; DestDir: "{app}/models"; CopyMode: alwaysoverwrite

; fonts
Source: "fonts\clean12.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\clean16.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\cleanbold12.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\cleanbold16.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\default.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\helv10.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\helv12.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\helv18.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\helv24.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\helvbold12.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\helvbold18.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\helvbold24.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite
Source: "fonts\sansbold20.txf"; DestDir: "{app}/fonts"; CopyMode: alwaysoverwrite

[INI]
Filename: "{app}\celestia.url"; Section: "InternetShortcut"; Key: "URL"; String: "http://www.shatters.net/celestia/"

[Icons]
Name: "{group}\Celestia"; Filename: "{app}\celestia.exe"; WorkingDir: "{app}"
Name: "{group}\README"; Filename: "{app}\README.txt"
Name: "{group}\Celestia on the Web"; Filename: "{app}\celestia.url"
Name: "{userdesktop}\Celestia"; Filename: "{app}\celestia.exe"; MinVersion: 4,4; Tasks: desktopicon; WorkingDir: "{app}"

[Registry]
Root: HKCR; Subkey: "cel"; ValueType: string; ValueData: "URL:cel Protocol"; Tasks: urlassoc
Root: HKCR; Subkey: "cel"; ValueName: "URL Protocol"; ValueType: string; Tasks: urlassoc
Root: HKCR; Subkey: "cel\Shell"; ValueType: string; Tasks: urlassoc
Root: HKCR; Subkey: "cel\Shell\open"; ValueType: string; Tasks: urlassoc
Root: HKCR; Subkey: "cel\Shell\open\Command"; ValueType: string; ValueData: "{app}\celestia.exe --once -dir ""{app}"" -u ""%1"""; Tasks: urlassoc

[Run]
Filename: "{app}\celestia.exe"; Description: "Launch Celestia"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\celestia.url"

