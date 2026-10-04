# Copy to local.psd1 (git-ignored) and fill in paths on your machine. Parameters and D2H_<Name>
# environment variables override these. Never point anything inside this repository at game data.
@{
	DataDir     = 'C:\Games\DevilutionX'                      # your DIABDAT.MPQ and devilutionx.mpq
	D2CommonDll = 'C:\Games\Diablo II\D2Common.dll'            # Diablo 2 1.12, reference for the parity checks
	StockExe    = 'C:\src\devilutionx-1.5.3\build-stock\Release\devilutionx.exe'  # stock build for FPS baseline
	OutDir      = 'C:\Games\DevilutionX-D2-Movement'           # play folder made by package.ps1
	# CMake     = 'C:\Program Files\CMake\bin\cmake.exe'       # only if cmake is not on PATH
}
