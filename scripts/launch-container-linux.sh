#!/bin/bash

docker run -it \
    -e DISPLAY=$DISPLAY \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    --name lsdslam_noros \
    lsdslam_noros