# Legal

## What is in this repository

Only original code written for this project: the ARM64 → C translator, the
Mach-O and Unity asset readers, the Windows (and Android) runtime and
compatibility layers, build scripts and documentation.

There is **no** game data here: no IPA, no `UnityFramework`, no
`global-metadata.dat`, no textures, models, audio, video, shaders or text, and
no bytes extracted from any of them. The code names a small number of the
game's methods and addresses where it has to replace or trace them; it
contains no disassembly listings, symbol tables or string tables of the game.

## What you need to supply

Your own copy of the iOS app *Marble Knights* 2.3.1 (build 760642). Whether and
how you may obtain or keep a copy of an app that is no longer sold depends on
the law where you live. This project takes no position on it and provides no
app, no link to one and no help finding one.

To build from source you also need Unity 2019.4.17f1 (the Editor and Windows
Build Support), which you download from Unity yourself under Unity's terms.

## What the build produces, and what the releases will contain

The build writes C into `work/win/gen`: a machine translation of the ARM64
code in `UnityFramework`. That code is the game's compiled C#, plus the parts
of the iOS app it runs on that were linked into the same binary — Unity's
IL2CPP runtime and the FMOD audio engine — much as an Xbox 360 executable
carries the libraries and middleware it was linked with. It is compiled into
`GameAssembly.dll`.

It is **not** the game. It contains no textures, models, audio, video,
shaders, scenes or text, and it does nothing without the game's own data
(`global-metadata.dat`, the scenes and the asset bundles), which come only
from your copy of the app.

When this project publishes releases, they will contain:

- our own code (the launcher and the runtime);
- the recompiled `GameAssembly.dll`;
- the official Unity 2019.4.17f1 player files the game runs on
  (`UnityPlayer.dll` and the player executable; for Android, Unity's Android
  player).

The launcher takes your copy of the app and builds the rest on your computer:
it extracts the game's data, converts its shaders and textures, and assembles
the playable folder (or, for Android, the APK). This follows the practice of
other static recompilation projects — [Unleashed
Recompiled](https://github.com/hedge-dev/UnleashedRecomp) and
[Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp) — which
distribute the recompiled code and require each user to bring their own copy
of the game.

**Be aware that this position is not settled law.** A mechanical translation
of a program is reasonably argued to be a derivative work of it, and rights
holders have issued takedowns against comparable projects. This project ships
these builds on the view that the recompiled code is an interoperability work
that is inert without the user's own game data, but nobody should treat that
as a guarantee. If you redistribute a build of this project, you do so on your
own judgement and at your own risk. If a rights holder asks for something to
be taken down, it will be.

What remains off limits, without qualification:

- The IPA itself, in any form, whole or in part.
- Game assets extracted from it: textures, models, audio, video, shaders,
  scenes, text, `global-metadata.dat`.
- Committing the generated C, a built `GameAssembly.dll` or any of the above
  to this repository.
- Opening issues or pull requests containing any of the above.

## Trademarks

*Marble Knights* is a trademark of WayForward Technologies, Inc. *Apple
Arcade*, *iOS* and *Game Center* are trademarks of Apple Inc. *Unity* is a
trademark of Unity Technologies. *FMOD* is a trademark of Firelight
Technologies. This project is not affiliated with, endorsed by, or supported
by any of them; the names are used only to say what the software is
compatible with.

## No warranty

This is a hobby preservation and reverse-engineering project, provided as is.
See [LICENSE](LICENSE) for the exact terms.
