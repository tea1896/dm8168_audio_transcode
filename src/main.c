
#include <signal.h>
#include "ti_vsys.h"
#include "ti_venc.h"
#include "ti_vdis.h"

Bool startAudioDecodeSystem(Void);

Int main(Int argc, Char **argv) {
    /* init system */
    VSYS_PARAMS_S vsysParams;
    Vsys_params_init(&vsysParams);
    Vsys_init(&vsysParams);

    // Start to decode audio.
    startAudioDecodeSystem();

    Vsys_exit();

    return 0;
}
