@mainpage

staticStackAnalyzer
===================

Abstract
--------

This program parses an ELF file and calculates the estimated stack usage of it.
It is build for ELF files compiled by mips gcc in general and xc32 from
Microchip Technology Inc. in special with MIPS32 release 5 target architecture.
(This should apply to whole PIC32MZ-family, maybe even more)

After parsing the ELF file, this function prints a markdown compatible table
of the results, sorted and limited for your needs. This can be directly
piped into a *.md file, so doxygen can include it into your program documentation.


How to use this program
-----------------------

- Compile it with your gcc, use build.bat or use it as an example.
- Run the program in your console and read the help message.


Under the hood
--------------

- xc32-objdump is invoked to get the disassembly of an ELF file.
- This disassembly is then parsed for magic patterns.
  * "Disassembly of section .text" to find the start of any text section (also .text.*).
  * "</labelname/>" for function names, where "<.xxxy>" is interpreted as compiler internal label and ignored
  * " \taddiu\tsp,sp," to find stack pointer modification.
  * " \tb" and " \tj" for any jumps.
    - "jr" semms to be switch case, those are ignored.
    - "jalr ra" are return jumps, ignored.
    - any other "jalr" are indirect jumps in c-code, which cannot be traced.
- The list of jumps is then used to determine the deepest stack usage.
- The resulting list of function is then sorted and printed.


Known limitations
-----------------

- The program cannot handle recursive calls in your ELF. There is no fix for it at the moment. Recursive calls are completely ignored for now and result in 0 bytes stack usage.
- Assembler files by nature have no general pattern in their program flow, hence parsing them will be inacurate. An example is Microchip's startup file "crt0.S".


Help! The program does not work for me!
---------------------------------------

Here a list of ideas, how you can help yoursef:
- "xxx is not recognized as an internal or external command, ..."
  * Check ::openDisassembly if the path to your xc32-objdump.exe is correct. The installation folder and/or the version number might be different.
- The result list is empty or way too short
  * Check where your code is linked to. Change the section pattern, if your output section is not .text or .text.* (currently in ::main and ::findNextTextSection, this part of code needs refactoring)
  * Do not strip symbol information from final ELF file by linker. These are needed to determine start and end of functions
  * Without any parameters the result output is limited (see ::main or read help message for actual default limit). Use parameter -n-1 for unlimited result output.

more to follow...


Contact
-------

This program was developed by Magnetic Sense GmbH, Kelterstraße 59, D-72669 Unterensingen, https://magnetic-sense.com/.

Author is Florian Kaup, Sentinel Software GmbH, Weiherstraße 5, D-88682 Salem, https://www.sentinelsw.de/.

Published under GNU General Public License v3 (https://www.gnu.org/licenses/gpl-3.0) with permission from Markus Lang, Magnetic Sense GmbH

Copyright (c) Magnetic Sense GmbH, 2020
