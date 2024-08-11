#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_TRACKS 10

typedef struct {
    bool stopped;
    bool playing;
    bool paused;
    int currentTrack;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AudioControl;

void *audioThread(void *arg) {
    AudioControl *control = (AudioControl *)arg;

    for (int i = 0; i < 5; ++i) {

        if (control->stopped == true) {
            break;
        }

        while (control->stopped == false && control->paused == true) {
            pthread_cond_wait(&control->cond, &control->mutex);
        }

        printf("Playing track %d\n", control->currentTrack);
        sleep(2);

        if (control->playing == true && control->paused == false) {
            control->currentTrack = (control->currentTrack + 1) % MAX_TRACKS;
        }
    }

    return NULL;
}

int main() {
    AudioControl control = {0};
    pthread_t audioThreadId;
    char command;

    pthread_mutex_init(&control.mutex, NULL);
    pthread_cond_init(&control.cond, NULL);

    pthread_create(&audioThreadId, NULL, audioThread, &control);

    do {
        printf(
            "\nEnter command (p: play, s: stop, P: pause, n: next, r: prev): ");
        scanf(" %c", &command);

        switch (command) {
        case 'p':
            control.playing = true;
            control.paused = false;
            control.stopped = false;
            pthread_create(&audioThreadId, NULL, audioThread, &control);
            break;
        case 's':
            control.playing = false;
            control.stopped = true;
            break;
        case 'P':
            control.paused = !control.paused;
            control.stopped = false;
            break;
        case 'n':
            control.stopped = true;
            pthread_join(audioThreadId, NULL);
            control.currentTrack = (control.currentTrack + 1) % MAX_TRACKS;
            control.playing = true;
            control.paused = false;
            control.stopped = false;
            break;
        case 'r':
            control.currentTrack =
                (control.currentTrack - 1 + MAX_TRACKS) % MAX_TRACKS;
            control.playing = true;
            control.paused = false;
            control.stopped = false;
            break;
        }
    } while (1);
}
