# libraspilcd

This is a library/program for accessing a Display circuit for the Raspberry Pi
[Raspi-LCD](http://www.emsystech.de/produkt/raspi-lcd/), created by Martin
Steppuhn. This repository includes the example code from this project which is
released under GPL v2 by his author. This sourcecode is also available as a zip
file on the project page.

## Installation

It works only on the Raspberry Pi with the circuit above and requires `root`
privileges for accessing `/dev/mem`. Execute the following command to build and
execute the example program which shows some features:

    $ ./buildrun.sh

## Shared Library

You can compile the included functions into a shared library so that you can
use it in other programs or programming languages through libffi.

    $ make lib

This creates a `libraspilcd.so` file in the project directory, which includes
all the required code and data for this project.
