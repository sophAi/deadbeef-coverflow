/*
   ddb_dsp_libretro resampler plugin for DeaDBeeF player
   Copyright (C) 2021 Michael Lelli <toadking@toadking.com>
   based on SRC resampler plugin code
   Copyright (C) 2009-2022 Oleksiy Yakovenko <waker@users.sourceforge.net>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
#include "sinc_resampler.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <deadbeef/deadbeef.h>

static DB_dsp_t *_plugin;

#define trace(...) { deadbeef->log_detailed (&_plugin->plugin, 0, __VA_ARGS__); }

static DB_functions_t *deadbeef;

enum {
    LIBRETRO_PARAM_SAMPLERATE = 0,
    LIBRETRO_PARAM_QUALITY = 1,
    LIBRETRO_PARAM_AUTOSAMPLERATE = 2,
    LIBRETRO_PARAM_COUNT
};

#define LIBRETRO_BUFFER 16000
#define LIBRETRO_MAX_CHANNELS 8

typedef struct {
    ddb_dsp_context_t ctx;

    int channels;
    enum resampler_quality quality;
    float samplerate;
    int autosamplerate;
    rarch_sinc_resampler_t *resampler;
    int in_samplerate;
    int out_samplerate;
    int remaining; // number of input samples in the buffer
    float *outbuf;
    int outsize;
    int buffersize;
    __attribute__((__aligned__(16))) char in_fbuffer[sizeof(float)*LIBRETRO_BUFFER*LIBRETRO_MAX_CHANNELS];
    unsigned quality_changed : 1;
    unsigned need_reset : 1;
} ddb_libretro_t;

static void
ddb_libretro_close (ddb_dsp_context_t *_libretro) {
    ddb_libretro_t *libretro = (ddb_libretro_t*)_libretro;
    if (libretro->resampler) {
        resampler_sinc_free (libretro->resampler);
        libretro->resampler = NULL;
    }
    if (libretro->outbuf) {
        free (libretro->outbuf);
        libretro->outbuf = NULL;
    }
    free (libretro);
}

static void
ddb_libretro_reset (ddb_dsp_context_t *_libretro) {
    ddb_libretro_t *libretro = (ddb_libretro_t*)_libretro;
    libretro->need_reset = 1;
}

static int
_get_target_samplerate (ddb_libretro_t *libretro, ddb_waveformat_t *fmt) {
    if (libretro->autosamplerate) {
        DB_output_t *output = deadbeef->get_output ();
        return output->fmt.samplerate;
    }
    else {
        return libretro->samplerate;
    }
}

static int
ddb_libretro_can_bypass (ddb_dsp_context_t *_libretro, ddb_waveformat_t *fmt) {
    ddb_libretro_t *libretro = (ddb_libretro_t*)_libretro;

    int samplerate = _get_target_samplerate(libretro, fmt);

    return fmt->samplerate == samplerate;
}

static int
ddb_libretro_process (ddb_dsp_context_t *_libretro, float *samples, int nframes, int maxframes, ddb_waveformat_t *fmt, float *r) {
    ddb_libretro_t *libretro = (ddb_libretro_t*)_libretro;

    int samplerate = _get_target_samplerate(libretro, fmt);

    if (fmt->samplerate == samplerate) {
        return nframes;
    }

    int new_samplerates = libretro->in_samplerate != fmt->samplerate || libretro->out_samplerate != samplerate;

    libretro->in_samplerate = fmt->samplerate;
    libretro->out_samplerate = samplerate;

    fmt->samplerate = samplerate;

    if (libretro->need_reset || new_samplerates || libretro->channels != fmt->channels || libretro->quality_changed || !libretro->resampler) {
        libretro->quality_changed = 0;
        libretro->remaining = 0;
        libretro->channels = fmt->channels;
        if (libretro->resampler) {
            resampler_sinc_free(libretro->resampler);
            libretro->resampler = nullptr;
        }
        libretro->resampler = (rarch_sinc_resampler_t *)resampler_sinc_new (libretro->in_samplerate, libretro->out_samplerate, libretro->channels, libretro->quality);

        if (libretro->resampler == NULL) {
            trace("libretro_process failed to create resampler\n"
                    "in_samplerate=%d, out_samplerate=%d, channels=%d, quality=%d\n", libretro->in_samplerate, libretro->out_samplerate, libretro->channels, libretro->quality);
            return 0;
        }

        libretro->need_reset = 0;
    }

    int numoutframes = 0;
    int outsize = nframes*24;
    int buffersize = outsize * fmt->channels * sizeof (float);
    if (!libretro->outbuf || libretro->outsize != outsize || libretro->buffersize != buffersize) {
        if (libretro->outbuf) {
            free (libretro->outbuf);
            libretro->outbuf = NULL;
        }
        libretro->outsize = outsize;
        libretro->buffersize = buffersize;
        libretro->outbuf = (float*)malloc (buffersize);
    }
    char *output = (char *)libretro->outbuf;
    memset (output, 0, buffersize);
    float *input = samples;
    int inputsize = nframes;

    int samplesize = fmt->channels * sizeof (float);

    do {
        // add more frames to input buffer
        int n = inputsize;
        if (n >= LIBRETRO_BUFFER - libretro->remaining) {
            n = LIBRETRO_BUFFER - libretro->remaining;
        }

        if (n > 0) {
            memcpy (&libretro->in_fbuffer[libretro->remaining*samplesize], samples, n * samplesize);

            libretro->remaining += n;
            samples += n * fmt->channels;
        }
        if (!libretro->remaining) {
            trace ("WARNING: LIBRETRO input buffer starved\n");
            break;
        }

        // call resampler
        struct resampler_data data = {0};
        data.data_in = (float *)libretro->in_fbuffer;
        data.data_out = (float *)output;
        data.input_frames = libretro->remaining;
        libretro->resampler->process(libretro->resampler, &data);

        inputsize -= n;
        output += data.output_frames * samplesize;
        numoutframes += data.output_frames;
        outsize -= data.output_frames;

        // calculate how many unused input samples left
        libretro->remaining -= data.input_frames;
        // copy spare samples for next update
        if (libretro->remaining > 0 && data.input_frames > 0) {
            memmove (libretro->in_fbuffer, &libretro->in_fbuffer[data.input_frames*samplesize], libretro->remaining * samplesize);
        }
        if (data.output_frames == 0) {
            trace ("dsp_libretro: output_frames_gen=0, interrupt\n");
            break;
        }
    } while (inputsize > 0 && outsize > 0);

    memcpy (input, libretro->outbuf, numoutframes * fmt->channels * sizeof (float));
    //static FILE *out = NULL;
    //if (!out) {
    //    out = fopen ("out.raw", "w+b");
    //}
    //fwrite (input, 1,  numoutframes*sizeof(float)*(*nchannels), out);

    return numoutframes;
}

static int
ddb_libretro_num_params (void) {
    return LIBRETRO_PARAM_COUNT;
}

static const char *
ddb_libretro_get_param_name (int p) {
    switch (p) {
    case LIBRETRO_PARAM_QUALITY:
        return "Quality";
    case LIBRETRO_PARAM_SAMPLERATE:
        return "Samplerate";
    case LIBRETRO_PARAM_AUTOSAMPLERATE:
        return "Auto samplerate";
    default:
        trace("ddb_libretro_get_param_name: invalid param index (%d)\n", p);
    }
    return NULL;
}

static void
ddb_libretro_set_param (ddb_dsp_context_t *ctx, int p, const char *val) {
    switch (p) {
    case LIBRETRO_PARAM_SAMPLERATE:
        ((ddb_libretro_t*)ctx)->samplerate = atof (val);
        if (((ddb_libretro_t*)ctx)->samplerate < 8000) {
            ((ddb_libretro_t*)ctx)->samplerate = 8000;
        }
        if (((ddb_libretro_t*)ctx)->samplerate > 192000) {
            ((ddb_libretro_t*)ctx)->samplerate = 192000;
        }
        break;
    case LIBRETRO_PARAM_QUALITY:
        ((ddb_libretro_t*)ctx)->quality = (enum resampler_quality) atoi (val);
        ((ddb_libretro_t*)ctx)->quality_changed = 1;
        break;
    case LIBRETRO_PARAM_AUTOSAMPLERATE:
        ((ddb_libretro_t*)ctx)->autosamplerate = atoi (val);
        break;
    default:
        trace("ddb_libretro_set_param: invalid param index (%d)\n", p);
    }
}

static void
ddb_libretro_get_param (ddb_dsp_context_t *ctx, int p, char *val, int sz) {
    switch (p) {
    case LIBRETRO_PARAM_SAMPLERATE:
        snprintf (val, sz, "%f", ((ddb_libretro_t*)ctx)->samplerate);
        break;
    case LIBRETRO_PARAM_QUALITY:
        snprintf (val, sz, "%d", ((ddb_libretro_t*)ctx)->quality);
        break;
    case LIBRETRO_PARAM_AUTOSAMPLERATE:
        snprintf (val, sz, "%d", ((ddb_libretro_t*)ctx)->autosamplerate);
        break;
    default:
        trace("ddb_libretro_get_param: invalid param index (%d)\n", p);
    }
}

static const char settings_dlg[] =
    "property \"Autodetect samplerate from output device\" checkbox 2 0;\n"
    "property \"Set samplerate directly\" spinbtn[8000,192000,1] 0 44100;\n"
    "property \"Resampler quality\" select[5] 1 2 Lowest Lower Normal Higher Highest;\n"
;

static ddb_dsp_context_t *ddb_libretro_open(void);

static DB_dsp_t plugin = {
    .plugin = {
        .type = DB_PLUGIN_DSP,
        .api_vmajor = DB_API_VERSION_MAJOR,
        .api_vminor = DB_API_VERSION_MINOR,
        .version_major = 1,
        .version_minor = 0,
        .flags = DDB_PLUGIN_FLAG_LOGGING,
        .id = "resampler_libretro",
        .name = "Resampler (Libretro)",
        .descr =
            "Samplerate converter using the sinc resampler from Libretro/RetroArch.\n"
            "\n"
            "Based on foobar2000 modifications:\n"
            "https://www.foobar2000.org/sinc-resampler",
        .copyright =
            "ddb_dsp_libretro resampler plugin for DeaDBeeF player\n"
            "Copyright (C) 2021 Michael Lelli <toadking@toadking.com>\n"
            "based on SRC resampler plugin code\n"
            "Copyright (C) 2009-2022 Oleksiy Yakovenko <waker@users.sourceforge.net>\n"
            "\n"
            "This program is free software; you can redistribute it and/or\n"
            "modify it under the terms of the GNU General Public License\n"
            "as published by the Free Software Foundation; either version 2\n"
            "of the License, or (at your option) any later version.\n"
            "\n"
            "This program is distributed in the hope that it will be useful,\n"
            "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
            "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
            "GNU General Public License for more details.\n"
            "\n"
            "You should have received a copy of the GNU General Public License\n"
            "along with this program; if not, write to the Free Software\n"
            "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
            "\n"
            "\n"
            "sinc_resampler.h\n"
            "Copyright  (C) 2010-2018 The RetroArch team\n"
            "\n"
            "Permission is hereby granted, free of charge,\n"
            "to any person obtaining a copy of this software and associated documentation files (the \"Software\"),\n"
            "to deal in the Software without restriction, including without limitation the rights to\n"
            "use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,\n"
            "and to permit persons to whom the Software is furnished to do so, subject to the following conditions:\n"
            "\n"
            "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\n"
            "\n"
            "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,\n"
            "INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
            "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.\n"
            "IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,\n"
            "WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
            "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n"
            "\n"
            "\n"
            "Modified by Janne Hyvärinen\n"
            "Modified some more by Peter Pawlowski\n"
            "\n"
            "\n"
            "\n"
            "ppsimd\n"
            "Copyright (C) 2002-2021 Peter Pawlowski\n"
            "\n"
            "This software is provided 'as-is', without any express or implied\n"
            "warranty.  In no event will the authors be held liable for any damages\n"
            "arising from the use of this software.\n"
            "\n"
            "Permission is granted to anyone to use this software for any purpose,\n"
            "including commercial applications, and to alter it and redistribute it\n"
            "freely, subject to the following restrictions:\n"
            "\n"
            "1. The origin of this software must not be misrepresented; you must not\n"
            "   claim that you wrote the original software. If you use this software\n"
            "   in a product, an acknowledgment in the product documentation would be\n"
            "   appreciated but is not required.\n"
            "2. Altered source versions must be plainly marked as such, and must not be\n"
            "   misrepresented as being the original software.\n"
            "3. This notice may not be removed or altered from any source distribution.\n"
        ,
        .website = "https://github.com/ToadKing/ddb_dsp_libretro",
    },
    .open = ddb_libretro_open,
    .close = ddb_libretro_close,
    .process = ddb_libretro_process,
    .reset = ddb_libretro_reset,
    .num_params = ddb_libretro_num_params,
    .get_param_name = ddb_libretro_get_param_name,
    .set_param = ddb_libretro_set_param,
    .get_param = ddb_libretro_get_param,
    .configdialog = settings_dlg,
    .can_bypass = ddb_libretro_can_bypass,
};

extern "C" DB_plugin_t *ddb_dsp_libretro_load (DB_functions_t *f);

DB_plugin_t *
ddb_dsp_libretro_load (DB_functions_t *f) {
    deadbeef = f;
    _plugin = &plugin;
    return &plugin.plugin;
}

ddb_dsp_context_t*
ddb_libretro_open (void) {
    ddb_libretro_t *libretro = (ddb_libretro_t*)malloc (sizeof (ddb_libretro_t));
    DDB_INIT_DSP_CONTEXT (libretro,ddb_libretro_t, &plugin);

    libretro->autosamplerate = 0;
    libretro->samplerate = 44100;
    libretro->quality = RESAMPLER_QUALITY_NORMAL;
    libretro->channels = -1;
    return (ddb_dsp_context_t *)libretro;
}
