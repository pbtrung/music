#!/bin/ash

snapserver --config "$1" &
music "$2" | gwsocket --port=9000 --addr=0.0.0.0 --std
