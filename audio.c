#include <stdbool.h>
#include <gst/gst.h>
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>

#define I2C_FILENAME "/dev/i2c-1"
#define I2C_SLAVE_ADDR 0x08
#define I2C_SLAVE_PAYLOAD_SIZE 6
#define LOOP_PERIOD 1000 //loop over fsm every 1 ms
#define LOOP_SPEEDUP_PERIOD 100000

#define RATE_UPPER_LIMIT 1.08
#define RATE_LOWER_LIMIT 0.92

static volatile bool INPUT_PLAY = false;
static volatile bool INPUT_STOP = false;

static volatile unsigned int INPUT_ANALOG_X = 0;
static volatile unsigned int INPUT_ANALOG_Y = 0;

typedef enum _FSM {
    FSM_INIT,
    FSM_IDLE,
    FSM_START,
    FSM_RUNNING,
    FSM_PAUZE,
    FSM_STOP,
    FSM_EXIT
} FSM;


/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData {
    GstElement *pipeline;
    GstElement *decode;
    GstElement *source;
    GstElement *convert;
    GstElement *speed;
    GstElement *sink;
    gboolean playing;      /* Are we in the PLAYING state? */
    gboolean terminate;    /* Should we terminate execution? */
    gboolean seek_enabled; /* Is seeking enabled for this media? */
    gboolean seek_done;    /* Have we performed the seek already? */
    gint64 duration;       /* How long does this media last, in nanoseconds */
    GstBus *bus;
    gdouble rate;
} CustomData;

/* Handler for the pad-added signal */
static void pad_added_handler (GstElement *src, GstPad *pad, CustomData *data);

/* Forward definition of the message processing function */
static void handle_message (CustomData *data, GstMessage *msg);


static int analog_direction(unsigned int analog_value) {
    if (analog_value > 150) {
        return 1;
    } else if (analog_value < 110) {
        return -1;
    } else {
        return 0;
    }
}

static bool read_inputs(CustomData *data) {
    bool retval = false;
    int fd = 0;
    unsigned char buf[I2C_SLAVE_PAYLOAD_SIZE];

    memset(buf, 0, I2C_SLAVE_PAYLOAD_SIZE);

    if ((fd = open(I2C_FILENAME, O_RDWR)) < 0) {
        g_printerr ("Failed to open i2c port.\n");
        return false;
    }

    if (ioctl(fd, I2C_SLAVE, I2C_SLAVE_ADDR) < 0) {
        g_printerr ("Unable to get bus access to talk to slave\n");
        goto exit;
    }

    buf[0] = 0;     // This is the register we want to read from

    if ((write(fd, buf, 1)) != 1) {     // Send register we want to read from
        g_printerr ("Error writing to i2c slave\n");
        goto exit;
    }

    if (read(fd, buf, I2C_SLAVE_PAYLOAD_SIZE) != I2C_SLAVE_PAYLOAD_SIZE) {    // Read back data into buf[]
        g_printerr ("Unable to read  header from slave\n");
        goto exit;
    }
    else {
        /* handle the buttons */
        if (buf[4] == 0x1) {
            INPUT_PLAY = true;
        } else {
            INPUT_PLAY = false;
        }
        if (buf[5] == 0x1) {
            INPUT_STOP = true;
        } else {
            INPUT_STOP = false;
        }
        /* handle the analog inputs */

        //Handle speed control
        int direction = analog_direction(buf[2]);
        if (direction != 0) {
            data->rate = data->rate + (direction * 0.001);
            //Limit range between +8+ and -8%
            if (data->rate >= RATE_UPPER_LIMIT) {
                data->rate = RATE_UPPER_LIMIT;
            } else if (data->rate <= RATE_LOWER_LIMIT) {
                data->rate = RATE_LOWER_LIMIT;
            }
        }
    }

    retval = true;
exit:
    close(fd);
    return retval;
}

static bool init_player(CustomData * data, const char* song_path) {

    /* Create the elements */
    data->source = gst_element_factory_make ("filesrc", "source");
    data->decode = gst_element_factory_make ("decodebin", "decoder");
    data->convert = gst_element_factory_make ("audioconvert", "convert");
    data->speed = gst_element_factory_make ("speed", "speed");
    data->sink = gst_element_factory_make ("autoaudiosink", "sink");

    /* Create the empty pipeline */
    data->pipeline = gst_pipeline_new ("test-pipeline");

    if (!data->pipeline || !data->source || !data->decode || !data->convert || !data->speed ||!data->sink) {
        g_printerr ("Not all elements could be created.\n");
        return false;
    }

    /* Build the pipeline. Note that we are NOT linking the source at this
   * point. We will do it later. */
    gst_bin_add_many (GST_BIN (data->pipeline), data->source, data->decode, data->convert, data->speed, data->sink, NULL);
    if (!gst_element_link_many (data->convert, data->speed, data->sink, NULL)) {
        g_printerr ("End Elements could not be linked.\n");
        gst_object_unref (data->pipeline);
        return false;
    }

    if (!gst_element_link (data->source, data->decode)) {
        g_printerr ("Begin Elements could not be linked.\n");
        gst_object_unref (data->pipeline);
        return false;
    }

    /* Set the URI to play */
    g_object_set (data->source, "location", song_path, NULL);

    /* Set the URI to play */
    g_object_set (data->speed, "speed", data->rate, NULL);

    /* Connect to the pad-added signal */
    g_signal_connect (data->decode, "pad-added", G_CALLBACK (pad_added_handler), data);

    return true;
}

static bool start_player(CustomData *data) {
    GstStateChangeReturn ret;

    ret = gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Unable to set the pipeline to the playing state.\n");
        gst_object_unref (data->pipeline);
        return false;
    }

    return true;
}

static bool stop_player(CustomData *data) {
    GstStateChangeReturn ret;

    ret = gst_element_set_state (data->pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Unable to set the pipeline to the playing state.\n");
        gst_object_unref (data->pipeline);
        return false;
    }

    return true;
}

static bool set_position_player(CustomData *data, unsigned int position) {
    gst_element_seek_simple (data->pipeline, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, position * GST_SECOND);
    return true;
}

static bool handle_position_player(CustomData *data) {
    if (data->playing) {
        gint64 current = -1;

        /* Query the current position of the stream */
        if (!gst_element_query_position (data->pipeline, GST_FORMAT_TIME, &current)) {
            //            g_printerr ("Could not query current position.\n");
            //            return false;
        }

        /* If we didn't know it yet, query the stream duration */
        if (!GST_CLOCK_TIME_IS_VALID (data->duration)) {
            if (!gst_element_query_duration (data->pipeline, GST_FORMAT_TIME, &data->duration)) {
                //                g_printerr ("Could not query current duration.\n");
                //                return false;
            }
        }

        /* Print current position and total duration */
        g_print ("Position %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT " rate: %f\r", GST_TIME_ARGS (current), GST_TIME_ARGS (data->duration), data->rate);

    }
    return true;
}

static bool handle_message_player(CustomData *data) {
    GstMessage *msg = gst_bus_timed_pop_filtered (data->bus, 100 * GST_MSECOND,
                                                  GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_DURATION);

    /* Parse message */
    if (msg != NULL) {
        handle_message (data, msg);
    } else {
        /* We got no message, this means the timeout expired */
        if(!handle_position_player(data)) {
            return false;
        }
    }
    return true;
}

static bool fsm_player(FSM *fsm_state, CustomData *data) {
    switch (*fsm_state) {
    case FSM_INIT:
        //g_print ("State:[FSM_INIT]\n");
        *fsm_state = FSM_IDLE;
        INPUT_PLAY = true;
        break;
    case FSM_IDLE:
        //g_print ("State:[FSM_IDLE]\n");
        if (INPUT_PLAY == true) {
            *fsm_state = FSM_START;
            INPUT_PLAY = false;
        }
        break;
    case FSM_START:
        //g_print ("State:[FSM_START]\n");
        if (start_player(data)) {
            data->bus = gst_element_get_bus (data->pipeline);
            *fsm_state = FSM_RUNNING;
        } else {
            *fsm_state = FSM_IDLE;
        }
        break;
    case FSM_RUNNING:
        //g_print ("State:[FSM_RUNNING]\n");
        if (INPUT_STOP == true || data->terminate) {
            *fsm_state = FSM_STOP;
        } else if (INPUT_PLAY == true) {
            *fsm_state = FSM_PAUZE;
        }

        handle_message_player(data);

        break;
    case FSM_STOP:
        //g_print ("State:[FSM_STOP]\n");
        *fsm_state = FSM_IDLE;

        stop_player(data);
        set_position_player(data, 0);

        break;
    case FSM_PAUZE:
        //g_print ("State:[FSM_PAUZE]\n");
        *fsm_state = FSM_IDLE;

        stop_player(data);

        break;
    case FSM_EXIT:
        //g_print ("State:[FSM_EXIT]\n");

        break;
    default:
        break;
    }
    return true;
}

static void handle_message (CustomData *data, GstMessage *msg) {
    GError *err;
    gchar *debug_info;

    switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ERROR:
        gst_message_parse_error (msg, &err, &debug_info);
        g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
        g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
        g_clear_error (&err);
        g_free (debug_info);
        data->terminate = TRUE;
        break;
    case GST_MESSAGE_EOS:
        g_print ("End-Of-Stream reached.\n");
        data->terminate = TRUE;
        break;
    case GST_MESSAGE_DURATION:
        /* The duration has changed, mark the current one as invalid */
        data->duration = GST_CLOCK_TIME_NONE;
        break;
    case GST_MESSAGE_STATE_CHANGED: {
        GstState old_state, new_state, pending_state;
        gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
        if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
            g_print ("Pipeline state changed from %s to %s:\n",
                     gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));

            /* Remember whether we are in the PLAYING state or not */
            data->playing = (new_state == GST_STATE_PLAYING);

            if (data->playing) {
                /* We just moved to PLAYING. Check if seeking is possible */
                GstQuery *query;
                gint64 start, end;
                query = gst_query_new_seeking (GST_FORMAT_TIME);
                if (gst_element_query (data->pipeline, query)) {
                    gst_query_parse_seeking (query, NULL, &data->seek_enabled, &start, &end);
                    if (data->seek_enabled) {
                        g_print ("Seeking is ENABLED from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n", GST_TIME_ARGS (start), GST_TIME_ARGS (end));
                    } else {
                        g_print ("Seeking is DISABLED for this stream.\n");
                    }
                }
                else {
                    g_printerr ("Seeking query failed.");
                }
                gst_query_unref (query);
            }
        }
    } break;
    default:
        /* We should not reach here */
        g_printerr ("Unexpected message received.\n");
        break;
    }
    gst_message_unref (msg);
}

/* This function will be called by the pad-added signal */
static void pad_added_handler (GstElement *src, GstPad *new_pad, CustomData *data) {
    GstPad *sink_pad = gst_element_get_static_pad (data->convert, "sink");
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    g_print ("Received new pad '%s' from '%s':\n", GST_PAD_NAME (new_pad), GST_ELEMENT_NAME (src));

    /* If our converter is already linked, we have nothing to do here */
    if (gst_pad_is_linked (sink_pad)) {
        g_print ("We are already linked. Ignoring.\n");
        goto exit;
    }

    /* Check the new pad's type */
    new_pad_caps = gst_pad_get_current_caps (new_pad);
    new_pad_struct = gst_caps_get_structure (new_pad_caps, 0);
    new_pad_type = gst_structure_get_name (new_pad_struct);
    if (!g_str_has_prefix (new_pad_type, "audio/x-raw")) {
        g_print ("It has type '%s' which is not raw audio. Ignoring.\n", new_pad_type);
        goto exit;
    }

    /* Attempt the link */
    ret = gst_pad_link (new_pad, sink_pad);
    if (GST_PAD_LINK_FAILED (ret)) {
        g_print ("Type is '%s' but link failed.\n", new_pad_type);
    } else {
        g_print ("Link succeeded (type '%s').\n", new_pad_type);
    }

exit:
    /* Unreference the new pad's caps, if we got them */
    if (new_pad_caps != NULL)
        gst_caps_unref (new_pad_caps);

    /* Unreference the sink pad */
    gst_object_unref (sink_pad);
}

static long get_microtime(){
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);
    return currentTime.tv_sec * (int)1e6 + currentTime.tv_usec;
}

int main(int argc, char *argv[]) {
    CustomData data;

    data.playing = FALSE;
    data.terminate = FALSE;
    data.seek_enabled = FALSE;
    data.seek_done = FALSE;
    data.duration = GST_CLOCK_TIME_NONE;
    data.bus = NULL;
    data.rate = 1.0;
    FSM fsm_state = FSM_INIT;
    gdouble old_rate = 1.0;

    const char* song_path = NULL;

    long time_now;
    long time_previous;
    long time_speedup_previous;

    if (argc < 2) {
        g_printerr ("Not enough input arguments, please use audio_player <your mp3 file>.\n");
        return -1;
    }

    song_path = argv[1];

    /* Initialize GStreamer */
    gst_init (&argc, &argv);

    if (!init_player(&data, song_path)) {
        g_printerr ("Could not init player\n");
        return -1;
    }

    time_previous = get_microtime();
    time_speedup_previous = time_previous;

    while(fsm_state != FSM_EXIT) {
        time_now = get_microtime();
        if ((time_now - time_previous) > LOOP_PERIOD) {
            read_inputs(&data);
            if (!fsm_player(&fsm_state, &data)) {
                g_printerr ("fsm error\n");
                break;
            }
            time_previous = time_now;
        }

        if ((time_now - time_speedup_previous)> LOOP_SPEEDUP_PERIOD) {
            if(data.rate != old_rate) {
                g_object_set (data.speed, "speed", data.rate, NULL);
                old_rate = data.rate;
            }
            time_speedup_previous = time_now;
        }
    }

    /* Free resources */
    gst_element_set_state (data.pipeline, GST_STATE_NULL);
    gst_object_unref (data.pipeline);
    return 0;
}
