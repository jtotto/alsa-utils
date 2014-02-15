#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#define CONTROLLER_OFF 0
#define CONTROLLER_ON 127

// Obviously this must live in the main loop.  Macro'd because I couldn't bring myself to duplicate the function for rising/falling,
// but didn't want to incur the performance penalty of function calls.
#undef toggle_loop
#define toggle_loop( toggleCondition, onToggle )  \
while (1)   \
{ \
    if (poll(pfd, npfd, 1000000) > 0) \
    { \
        do  \
        { \
            bytesRemaining = snd_seq_event_input(seq_handle, &inputEvent); \
            if( inputEvent->type == SND_SEQ_EVENT_CONTROLLER &&  \
                inputEvent->data.control.channel == channel &&  \
                inputEvent->data.control.param == param \
            ) { \
                if( toggleCondition ) \
                { \
                    onToggle; \
                    snd_seq_event_output_direct( seq_handle, &outputEvent ); \
                } \
                previousValue = inputEvent->data.control.value; \
            } \
        }  \
        while (bytesRemaining > 0); \
    } \
} 

// Based heavily on tutorials at http://www.tldp.org/HOWTO/MIDI-HOWTO-9.html.
int main(int argc, char *argv[])
{
    // First read command line arguments.
    int opt;
    int directionFlag = 0, toggleValue = 64, channel = 0, param = 0, note = 0, argsFlag = 0, noteFlag = 0;
    while((opt = getopt(argc, argv, "c:p:n:t:r")) != -1)
    {  
        switch(opt)
        {  
            case 't': toggleValue = atoi(optarg); break;
            case 'c': channel = atoi(optarg); break;
            case 'p': param = atoi(optarg); break;
            case 'n': 
                {
                    note = atoi(optarg); 
                    noteFlag = 1;
                }
                break;
            case 'r': directionFlag = 1; break;
        }
        argsFlag = 1;
    }
    
    if( !argsFlag ) {
        printf( "This program acts as a MIDI CC toggle switch, toggling between 0 and 127 when input passes through a specified value.\n\n" );
        printf( "Usage:\n" );
        printf( "-t: set the value (inclusive) at which toggling occurs\n" );
        printf( "-c: set the MIDI channel on which to send and receive\n" );
        printf( "-p: set the MIDI CC parameter on which to send and receive\n" );
        printf( "-n: alternately, set the key on which to fire NOTE ON events (NO CORRESPONDING NOTE OFF MESSAGES WILL BE SENT!)\n" );
        printf( "    (if both are specified the -n setting will be used)\n" );
        printf( "-r: if this switch is included, toggling occurs when rising through the toggle value\n" );

        return 0;
    }
    if( toggleValue < 0 || 
        toggleValue > 127 ||
        channel < 0 ||
        channel > 15 ||
        param < 0 ||
        param > 127 ||
        note < 0 ||
        note > 127
    ) {
        printf("Invalid args.\n");
        exit(1);
    }

    // Next, initalise interaction with ALSA MIDI sequencer.
    snd_seq_t *seq_handle;
    snd_seq_event_t *inputEvent;
    snd_seq_event_t outputEvent;

    int portid;          /* input port */
    int oportid;         /* output port */
    int npfd;
    struct pollfd *pfd;

    if(snd_seq_open(&seq_handle, "hw", SND_SEQ_OPEN_DUPLEX, 0) < 0) 
    {
        printf("Error opening ALSA sequencer.\n");
        exit(1);
    }

    snd_seq_set_client_name(seq_handle, "CC Toggle Switch");

    /* open one input port */
    if ((portid = snd_seq_create_simple_port
                (seq_handle, "Input",
                 SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                 SND_SEQ_PORT_TYPE_APPLICATION)) < 0) {
        printf("fatal error: could not open input port.\n");
        exit(1);
    }
    /* open one output port */
    if ((oportid = snd_seq_create_simple_port
                (seq_handle, "Output",
                 SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                 SND_SEQ_PORT_TYPE_APPLICATION)) < 0) {
        printf("fatal error: could not open output port.\n");
        exit(1);
    }

    // These properties of the event don't change.
    snd_seq_ev_clear( &outputEvent );
    snd_seq_ev_set_source( &outputEvent, oportid );
    snd_seq_ev_set_subs( &outputEvent );
    snd_seq_ev_set_direct( &outputEvent );
    if( noteFlag )
    {
        snd_seq_ev_set_noteon( &outputEvent, channel, note, 60 );
    }
    else
    {
        snd_seq_ev_set_controller( &outputEvent, channel, param, CONTROLLER_OFF); // Initialize event in the off position.
    }
    // Prepare for the main loop polling.
    npfd = snd_seq_poll_descriptors_count(seq_handle, POLLIN);
    pfd = (struct pollfd *)alloca(npfd * sizeof(struct pollfd));
    snd_seq_poll_descriptors(seq_handle, pfd, npfd, POLLIN);

    int bytesRemaining = 0;
    int previousValue = directionFlag ? 0 : 127; 

    // Main loop macro invocations.
    if( noteFlag )
    {
        if( directionFlag )
        {
            toggle_loop( inputEvent->data.control.value >= toggleValue && previousValue < toggleValue, /* don't do anything */ );
        }
        else
        {
            toggle_loop( inputEvent->data.control.value <= toggleValue && previousValue > toggleValue, /* don't do anything */ );
        }
    }
    else
    {
        if( directionFlag )
        {
            toggle_loop( inputEvent->data.control.value >= toggleValue && previousValue < toggleValue, outputEvent.data.control.value = outputEvent.data.control.value == CONTROLLER_OFF ? CONTROLLER_ON : CONTROLLER_OFF );
        }
        else
        {
            toggle_loop( inputEvent->data.control.value <= toggleValue && previousValue > toggleValue, outputEvent.data.control.value = outputEvent.data.control.value == CONTROLLER_OFF ? CONTROLLER_ON : CONTROLLER_OFF);
        }
    }

    return 0;
}
