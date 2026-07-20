# WhittyArcade third-party notices

WhittyArcade contains components and adaptations that remain the property of
their respective copyright holders. The WhittyArcade proprietary licence does
not replace or restrict the licences below for those components.

This notice must accompany distributed WhittyArcade binaries. Source files
also retain their applicable notices alongside the code.

## BSD 3-Clause components

The following components are distributed under the BSD 3-Clause licence:

- MAME-derived System 22 behaviour and device adaptations, including work by
  Phil Stroffolino, hap, R. Belmont, superctr, Tony La Porta, and other MAME
  contributors.
- Intel i960 and NEC V60 CPU adaptations — Farfetch'd and R. Belmont.
- Mitsubishi M37710 CPU adaptation — R. Belmont and Karl Stenerud.
- Sega Model 1 TGP, video, and MB86233 adaptations — Olivier Galibert and
  other MAME contributors.
- MultiPCM/GEW adaptation — M. A. Horna and other MAME contributors.
- SegaPCM — Hiromitsu Shioya, Olivier Galibert, and other MAME contributors.
- ymfm — Copyright © 2021 Aaron Giles.

MAME project notice: Copyright © 1997–2026 MAMEDev and contributors.

BSD 3-Clause License

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

MAME licensing information: https://docs.mamedev.org/license.html

## Musashi

Musashi is Copyright Karl Stenerud. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Berkeley SoftFloat Release 2b

Portions of Musashi include Berkeley SoftFloat Release 2b by John R. Hauser.
The retained source carries this notice:

This C source file is part of the SoftFloat IEC/IEEE Floating-point Arithmetic
Package, Release 2b.

Written by John R. Hauser. This work was made possible in part by the
International Computer Science Institute, located at Suite 600, 1947 Center
Street, Berkeley, California 94704. Funding was partially provided by the
National Science Foundation under grant MIP-9311980. The original version of
this code was written as part of a project to build a fixed-point vector
processor in collaboration with the University of California at Berkeley,
overseen by Profs. Nelson Morgan and John Wawrzynek. More information is
available through the Web page
http://www.cs.berkeley.edu/~jhauser/arithmetic/SoftFloat.html.

THIS SOFTWARE IS DISTRIBUTED AS IS, FOR FREE. Although reasonable effort has
been made to avoid it, THIS SOFTWARE MAY CONTAIN FAULTS THAT WILL AT TIMES
RESULT IN INCORRECT BEHAVIOR. USE OF THIS SOFTWARE IS RESTRICTED TO PERSONS
AND ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ALL LOSSES,
COSTS, OR OTHER PROBLEMS THEY INCUR DUE TO THE SOFTWARE, AND WHO FURTHERMORE
EFFECTIVELY INDEMNIFY JOHN HAUSER AND THE INTERNATIONAL COMPUTER SCIENCE
INSTITUTE (possibly via similar legal warning) AGAINST ALL LOSSES, COSTS, OR
OTHER PROBLEMS INCURRED BY THEIR CUSTOMERS AND CLIENTS DUE TO THE SOFTWARE.

Derivative works are acceptable, even for commercial purposes, so long as
(1) the source code for the derivative work includes prominent notice that
the work is derivative, and (2) the source code includes prominent notice with
these four paragraphs for those parts of this code that are retained.

## Moira

Moira is Copyright © 2019–2023 Dirk W. Hoffmann and is distributed under the
MIT Licence. The Moira source and full licence are retained in
`third_party/moira/Moira/LICENSE`.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## chips Z80

The chips Z80 core is Copyright © 2021 Andre Weissflog and uses the
zlib/libpng licence:

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

## System libraries

The Linux executable dynamically links to system-provided libraries including
SDL, SDL_ttf, OpenAL, OpenGL/GLX, GLEW, Vulkan, zlib, minizip, mpg123,
FreeType, HarfBuzz, libpng, and their dependencies. Those libraries are not
included in the WhittyArcade release archive and remain under their own
licences as installed by the user's Linux distribution.
