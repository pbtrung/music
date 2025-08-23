#!/bin/ash

snapserver --config "$1" &

mkfifo /tmp/music_pipe
music "$2" > /tmp/music_pipe &
music_pid=$!
gwsocket --port=9000 --addr=0.0.0.0 --std < /tmp/music_pipe &
gwsocket_pid=$!

# Wait for music to exit
wait $music_pid
# Kill gwsocket when music exits
kill -TERM $gwsocket_pid 2>/dev/null
rm /tmp/music_pipe
