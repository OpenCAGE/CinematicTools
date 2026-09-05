## Alien: Isolation Cinematic Tools

This is a continuation of the Alien: Isolation Cinematic Tools by [Hattiwatti](https://github.com/Hattiwatti), with the intention of adding extra useful features for modding the game.

### How to build

The project must be built in release mode, for x86 systems. This is the default project configuration.

Open `Alien Isolation\CT_AlienIsolation.sln` in Visual Studio 2022 (v143 toolset) and restore the NuGet packages (Boost, DirectXTK and MinHook all come from NuGet - nothing needs placing next to the repository). From the command line:

```
nuget restore "Alien Isolation\CT_AlienIsolation.sln"
msbuild "Alien Isolation\CT_AlienIsolation.vcxproj" /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir=<full path to the Alien Isolation folder>\r
```

The DLL is written to `Build\CT_AlienIsolation.dll`.

### How to use

To hook the cinematic tools into Alien: Isolation, run the game, and then launch "inject.bat" in the root of the project.

This utilises [Injector](https://github.com/nefarius/Injector) - a project by [Benjamin Höglinger](https://github.com/nefarius) (thanks!).