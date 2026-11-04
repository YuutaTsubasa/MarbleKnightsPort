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

You also need Unity 2019.4.17f1 (the Editor and Windows Build Support), which
you download from Unity yourself under Unity's terms. This repository does not
contain or redistribute any part of it.

## What the build produces

The build writes C into `work/win/gen`: a machine translation of the ARM64
code in your copy of `UnityFramework`. Together with the game's data and
Unity's player it makes a playable folder in `work/win/MarbleKnights`.
**All of that is derived from copyrighted software and stays on your
computer.** This project publishes no builds, and none should be shared:

- The IPA or anything extracted from it, in any form, whole or in part.
- The generated C, the compiled `GameAssembly.dll`, or an assembled player
  folder.
- Unity's player files.
- Issues or pull requests containing any of the above.

## Trademarks

*Marble Knights* is a trademark of WayForward Technologies, Inc. *Apple
Arcade*, *iOS* and *Game Center* are trademarks of Apple Inc. *Unity* is a
trademark of Unity Technologies. This project is not affiliated with,
endorsed by, or supported by any of them; the names are used only to say what
the software is compatible with.

## No warranty

This is a hobby preservation and reverse-engineering project, provided as is.
See [LICENSE](LICENSE) for the exact terms.
