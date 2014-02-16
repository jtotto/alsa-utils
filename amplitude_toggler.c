#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <jack/jack.h>
#include <alsa/asoundlib.h>

#define SAMPLE_CHECK_INTERVAL 5

#define CONTROLLER_OFF 0
#define CONTROLLER_ON 127

// JACK data.
jack_port_t *input_port;
jack_client_t *client;

// ALSA data.
snd_seq_t *seq_handle;
snd_seq_event_t outputEvent;
int oportid;
int iportid; // Only used in learn mode.

// process callback data.
int (*toggle_condition)(jack_default_audio_sample_t, jack_default_audio_sample_t);
void (*on_toggle)();
jack_default_audio_sample_t previousValue;
jack_default_audio_sample_t falling, rising;

// Global function pointers used here must be assigned before the callback is invoked.
int process (jack_nframes_t nframes, void *arg)
{
    jack_default_audio_sample_t *in;

    in = jack_port_get_buffer (input_port, nframes);

    jack_nframes_t i;
    for( i = 0; i < nframes; i += SAMPLE_CHECK_INTERVAL )
    {
        if( toggle_condition( previousValue, in[i] ) )
        {
            on_toggle();
            previousValue = in[i];
            return 0; // Any more than once is probably unintentional.
        }
        else
        {
            previousValue = in[i];
        }
    }

    return 0;
}

// Condition functions.
int rising_condition(jack_default_audio_sample_t previous, jack_default_audio_sample_t current)
{
    return previous < rising && current >= rising;
}

int falling_condition(jack_default_audio_sample_t previous, jack_default_audio_sample_t current)
{
    return previous > falling && current <= falling;
}

int rising_and_falling_condition(jack_default_audio_sample_t previous, jack_default_audio_sample_t current)
{
    return rising_condition(previous, current) || falling_condition(previous, current);
}

// Toggle action functions.
void basic_action()
{
    snd_seq_event_output_direct( seq_handle, &outputEvent );
}

void cc_action()
{
    outputEvent.data.control.value = outputEvent.data.control.value == CONTROLLER_OFF ? CONTROLLER_ON : CONTROLLER_OFF;
    basic_action();
}

void jack_shutdown (void *arg)
{
    exit (1);
}

int main (int argc, char *argv[])
{
    // First, become a JACK client.  This code is pretty much verbatim from simple_client.c
    const char *client_name = "amplitude_toggle_switch";
    const char *server_name = NULL;

    jack_options_t options = JackNullOption;
    jack_status_t status;

    /* open a client connection to the JACK server */

    client = jack_client_open (client_name, options, &status, server_name);
    if (client == NULL) 
    {
        fprintf (stderr, "jack_client_open() failed, "
             "status = 0x%2.0x\n", status);
        if (status & JackServerFailed) {
            fprintf (stderr, "Unable to connect to JACK server\n");
        }
        exit (1);
    }
    if (status & JackServerStarted) {
        fprintf (stderr, "JACK server started\n");
    }
    if (status & JackNameNotUnique) {
        client_name = jack_get_client_name(client);
        fprintf (stderr, "unique name `%s' assigned\n", client_name);
    }

    jack_set_process_callback (client, process, 0);

    jack_on_shutdown (client, jack_shutdown, 0);

    input_port = jack_port_register (client, "input",
                     JACK_DEFAULT_AUDIO_TYPE,
                     JackPortIsInput, 0);

    if ( input_port == NULL ) {
        fprintf(stderr, "no more JACK ports available\n");
        exit (1);
    }

    // -------------------------------------------------------------------

    // Next, process command line arguments.
    int opt;
    int channel = 0, param = 0, note = -1, argsFlag = 0, learnFlag = 0;
    falling = 0.0f, rising = 0.0f;

    while((opt = getopt(argc, argv, "f:r:c:p:n:l")) != -1)
    {
        switch(opt)
        {
            case 'f': falling = atof(optarg); break;
            case 'r': rising = atof(optarg); break;
            case 'c': channel = atoi(optarg); break;
            case 'p': param = atoi(optarg); break;
            case 'n': note = atoi(optarg); break;
            case 'l': learnFlag = 1; break;
        }
        argsFlag = 1;
    }

    if( !argsFlag ) {
        printf( "This program generates simple MIDI messages based on changes in the amplitude of its audio input.\n\n" );
        printf( "Usage:\n" );
        printf( "-f: set the falling edge amplitude at which toggling occurs\n" );
        printf( "-r: set the rising edge amplitude at which toggling occurs\n" );
        printf( "-c: set the MIDI channel on which to send\n" );
        printf( "-p: set the MIDI CC parameter on which to send (ON = 127, OFF = 0)\n" );
        printf( "-n: alternately, set the key on which to fire NOTE ON events (NO CORRESPONDING NOTE OFF MESSAGES WILL BE SENT!)\n" );
        printf( "    (if both are specified the -n setting will be used)\n" );
        printf( "-l: 'learn' the MIDI NOTEON/CONTROLLER message to output from MIDI input\n" );
        printf( "    (listens/outputs on -c channel)\n" );

        return 1;
    }

    if( channel < 0 ||
        channel > 15 ||
        param < 0 ||
        param > 127 ||
        (note != -1 && (note < 0 || note > 127))
    ) {
        printf("Invalid args.\n");
        exit(1);
    }


    // -------------------------------------------------------------------

    // Next, initalise interaction with ALSA MIDI sequencer.

    if(snd_seq_open(&seq_handle, "hw", SND_SEQ_OPEN_DUPLEX, 0) < 0)
    {
        printf("Error opening ALSA sequencer.\n");
        exit(1);
    }

    snd_seq_set_client_name(seq_handle, "amplitude_toggle_switch");

    /* open one output port */
    if ((oportid = snd_seq_create_simple_port
                (seq_handle, "Output",
                 SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                 SND_SEQ_PORT_TYPE_APPLICATION)) < 0) {
        printf("fatal error: could not open output port.\n");
        exit(1);
    }

    if( learnFlag ) 
    {
        if ((iportid = snd_seq_create_simple_port
                (seq_handle, "Input",
                SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                SND_SEQ_PORT_TYPE_APPLICATION)) < 0) {
            printf("fatal error: could not open input port.\n");
            exit(1);
        }
    }

    // These properties of the event don't change.
    snd_seq_ev_clear( &outputEvent );
    snd_seq_ev_set_source( &outputEvent, oportid );
    snd_seq_ev_set_subs( &outputEvent );
    snd_seq_ev_set_direct( &outputEvent );

    // -------------------------------------------------------------------

    // Initialize the process callback's data.
    previousValue = 0.0f;

    if( note >= 0 && note <= 127 )
    {
        snd_seq_ev_set_noteon( &outputEvent, channel, note, /* velocity */ 60 );
        on_toggle = basic_action;
    }
    else
    {
        snd_seq_ev_set_controller( &outputEvent, channel, param, CONTROLLER_OFF); // Initialize event in the off position.
                                                                                  // Will be fired in learn mode if nothing has been learned.
        on_toggle = learnFlag ? basic_action : cc_action;
    }

    if( rising != 0.0f && falling != 0.0f )
    {
        toggle_condition = rising_and_falling_condition;
    }
    else if( rising != 0.0f )
    {
        toggle_condition = rising_condition;
    }
    else
    {
        toggle_condition = falling_condition;
    }

    /* keep running until stopped by the user */

    if (jack_activate (client)) {
        fprintf (stderr, "cannot activate client");
        exit (1);
    }


    if( learnFlag )
    {
        // Begin polling for events to learn.
        int npfd, bytesRemaining = 0;
        struct pollfd *pfd;
        snd_seq_event_t *inputEvent;

        npfd = snd_seq_poll_descriptors_count(seq_handle, POLLIN);
        pfd = (struct pollfd *)alloca(npfd * sizeof(struct pollfd));
        snd_seq_poll_descriptors(seq_handle, pfd, npfd, POLLIN);

        #undef assignCommonEventMembers
        #define assignEventMembers( outputEvent, inputEvent ) \
        do {\
        outputEvent.data = inputEvent->data; \
        outputEvent.type = inputEvent->type; \
        outputEvent.flags = inputEvent->flags; \
        outputEvent.tag = inputEvent->tag; \
        } while( 0 ) 

        while (1)
        {
            if (poll(pfd, npfd, 1000000) > 0)
            {
                do
                {
                    bytesRemaining = snd_seq_event_input(seq_handle, &inputEvent);

                    switch( inputEvent->type )
                    {
                        case SND_SEQ_EVENT_NOTEON:
                            {
                                if( inputEvent->data.note.channel == channel )
                                {
                                    assignEventMembers( outputEvent, inputEvent );
                                }
                            }
                            break;
                        case SND_SEQ_EVENT_CONTROLLER:
                            {
                                if( inputEvent->data.control.channel == channel )
                                {
                                    assignEventMembers( outputEvent, inputEvent );
                                }
                            }
                            break;
                    }
                }
                while (bytesRemaining > 0);
            }
        }
    }
    else
    {
        sleep (-1);
    }

    /* this is never reached but if the program
       had some other way to exit besides being killed,
       they would be important to call.
    */

    jack_client_close (client);
    exit (0);
}
