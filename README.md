# rbpi_dj_player
a simple audio player for the raspberry pi

## Getting Started

These instructions will let you make a simple audio player on the raspberry pi and let you play/stop the song and even control it's playback speed by buttons and sliders/joystick.
### Prerequisites

Install gstreamer plugins.

```
sudo apt-get install libgstreamer1.0-0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-doc gstreamer1.0-tools gstreamer1.0-x gstreamer1.0-alsa gstreamer1.0-gl gstreamer1.0-pulseaudio
```

Have the following connected to the i2c ports of the raspberry pi to controll the player

* [arduino_sensors_to_i2c](https://github.com/jonathaneeckhout/arduino_sensors_to_i2c) - The audio payer controls


### Installing

Enter the project directory and run the make file

```
make
```


## Run the program

Execute the following command

```
./audio_player <YOUR AUDIO FILE>
```

## Acknowledgments

* Thanks to arduino examples
* Thanks to gstreamer examples
