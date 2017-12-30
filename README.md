# Attiny861WavPlayer
This is a modification of Elm Chan's "255-Voice PCM Sound Generator", to be used in a Hoerbert Wav-Player.

## Filesystem
Should be a FAT filesystem. I just used FAT32 (on a 4GB and a 16GB SD), but FAT16 should work too I guess.

## File format
All the sound files should be wav 44.1 kHz 16bit. Ordinary CD formatting.

## File naming
All the sound files should be stored in the root directory. There is a simple naming convention to map the songs to a playlist and a position within the playlist. 101.wav is the first song of playlist 1, 102.wav is the second song of playlist 1, 201.wav is the first song of playlist 2, 703.wav is the third song of playlist 7, and so on...

## Storing the position
At the moment, there is a file needed called POSITION.DAT. Like all the other files, it should be stored in the root directory. The file should not be empty. It should be at least 2 bytes big (as the program memory is veeeeery limited, I tried to avoid every extra functionality that is solvable otherwise for now).

## LED connection
We have implemented the possibility to use backlit buttons. As we used just 8 buttons (plus 2 control buttons), we just implemented 8 of them, but as the communication to the leds is serial, it would be possible to use the original 9 buttons (plus 2 control buttons) backlit. The communication is the classical DATA/CLOCK/LATCH concept used in many led technology. We used a MAX6971, but many others would work too, at least with small adaptations. Pinning is: 
#define LED_DATA PA3
#define LED_CLK PA2
#define LED_LE PA1

## flashing
All the SPI pins of the Attiny on the Hoerbert are connected to some soldering pads at the border of the device. All you need is an adapter from the standard ICSP to the 6 pin SPI on the Hoerbert.

This is a pretty minimal documentation as I am not sure how many people will get involved with this. Write me if you need more information.
