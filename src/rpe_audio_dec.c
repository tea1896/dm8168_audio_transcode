
#include "ti_audio.h"
#include <osa_thr.h>
#include <ti/xdais/dm/iaudio.h>


#define MAX_INPUT_STR_SIZE      (128)
#define MAX_INPUT_BUFFER        (4*1024)
#define MAX_OUTPUT_BUFFER       (4*1024)
#define AUDIO_TSK_PRI           (17)
#define AUDIO_TSK_STACK_SIZE    (10*1024)
#define MAX_IN_SAMPLES          (1024)
#define SAMPLE_LEN              (2)


#define WV_MAX_AUDIO_NUM            12

typedef struct {
    Int8  inFile[MAX_INPUT_STR_SIZE];
    Int8  outFile[MAX_INPUT_STR_SIZE];
    Int8  outEncFile[MAX_INPUT_STR_SIZE];
    Int32 channelIndex;
    Int32 numChannels;
    Int32 decodeType;
} Audio_DecInfo;

Void *App_allocBuf(Int32 bufSize, Bool fromSharedRegion);
Void App_freeBuf (Void *buf, Int32 bufSize, Bool fromSharedRegion);
char getChar();
int  getIntValue(char *string, int minVal, int maxVal, int defaultVal);

static OSA_ThrHndl      gAppDecodeThread[WV_MAX_AUDIO_NUM];
static Bool             gAppDecodeThreadActive[WV_MAX_AUDIO_NUM] = {FALSE};
static Bool             gAppDecodeThreadExitFlag[WV_MAX_AUDIO_NUM] = {FALSE};
static Audio_DecInfo    gDecInfo;

static Void writeToPCMFile (FILE *outputFile, ADEC_PROCESS_PARAMS_S *pPrm)
{
    /* This is a work around to skip initial 2 invalid frames from decoder */
    static Int32 frameCount = 0;

    if (pPrm->numSamples <= 0)
        return;

    if (frameCount > 1)
    {
        if(pPrm->channelMode == IAUDIO_1_0)
        {
            fwrite(pPrm->outBuf.dataBuf, pPrm->bytesPerSample, pPrm->numSamples, outputFile);
        }
        else 
        {
            if(pPrm->pcmFormat == IAUDIO_INTERLEAVED)
            {
                fwrite(pPrm->outBuf.dataBuf, pPrm->bytesPerSample, pPrm->numSamples*2, outputFile);
            }
            else
            { 
                UInt32 i;
                UInt8 * outL = (UInt8 *) pPrm->outBuf.dataBuf;
                UInt8 * outR = (UInt8 *)((UInt8 *) pPrm->outBuf.dataBuf + 
                        (pPrm->numSamples * pPrm->bytesPerSample)) ;
                for(i=0;i<(pPrm->numSamples * pPrm->bytesPerSample); i+= pPrm->bytesPerSample)
                {
                    /*--------------------------------------------------------------*/
                    /* Write the left anf right channels                            */
                    /*--------------------------------------------------------------*/
                    fwrite(&outL[i], pPrm->bytesPerSample, 1, outputFile);
                    fwrite(&outR[i], pPrm->bytesPerSample, 1, outputFile);
                }
            }
        }
    }
    frameCount++;
}



static void printDecodeStreamParams (ADEC_PROCESS_PARAMS_S *pPrm)
{
    printf ("\n\n------------ Decoded Stream------------\n");

    if(pPrm->channelMode == IAUDIO_1_0)
    {
        printf ("Channel Mode = MONO\n");
    }
    else 
    {
        printf ("Channel Mode = STEREO, ");
        if(pPrm->pcmFormat == IAUDIO_INTERLEAVED)
            printf (" INTERLEAVED\n");
        else
            printf (" PLANAR\n");
    }
    printf ("\n\n");
}

static Void *App_decodeTaskFxn(Void * prm)
{
    FILE                  *in = NULL, *out = NULL;
    UInt8                 *inBuf = NULL;
    UInt8                 *outBuf = NULL;
    ADEC_PROCESS_PARAMS_S decPrm;
    Int32                 rdIdx, to_read, readBytes;
    ADEC_CREATE_PARAMS_S  adecParams;
    Void                  *decHandle = NULL;
    Int32                 frameCnt = 0, totalSamples = 0;
    Int32                 inBufSize, outBufSize;
    Audio_DecInfo         *info = prm;
    Bool                  isSharedRegion = FALSE;
    char                  threadName[128];
    Int32                 channelIndex = 0;

    
    channelIndex = info->channelIndex;
    snprintf(threadName,sizeof(threadName),"%s_%x",__func__,channelIndex);
    threadName[sizeof(threadName) - 1] = 0;
    OSA_printTID(threadName);

    if (!prm)
    {
        printf ("ADEC task failed <invalid param>...........\n");
        return NULL;
    }

    /* init encoder start */ 
    FILE                  *encOut = NULL;
    AENC_CREATE_PARAMS_S  aencParams;
    AENC_PROCESS_PARAMS_S encPrm;
    Void                  *encHandle = NULL;
    UInt8                 *encinBuf = NULL;
    UInt8                 *encoutBuf = NULL;
    Int32                 encInBytes, encInBufSize, encOutBufSize;
    memset(&aencParams, 0, sizeof(aencParams));
    memset(&encPrm, 0, sizeof(encPrm));

    aencParams.bitRate = 200000;
    aencParams.encoderType = AUDIO_CODEC_TYPE_AAC_LC;
    aencParams.numberOfChannels = 2;
    aencParams.sampleRate = 44100;

    encHandle = Aenc_create(&aencParams);
    if (encHandle)
    {
        printf ("AENC Create done...........\n");
    }
    else
    {
        printf ("AENC Create failed...........\n");
        printf ("\n********Encode APP Task Exitting....\n");
        gAppDecodeThreadActive[channelIndex] = FALSE;
        gAppDecodeThreadExitFlag[channelIndex] = TRUE;
        return NULL;
    }
    gAppDecodeThreadExitFlag[channelIndex] = FALSE;

    if (aencParams.encoderType == AUDIO_CODEC_TYPE_AAC_LC)
    {
        encInBufSize = (MAX_IN_SAMPLES * SAMPLE_LEN * aencParams.numberOfChannels);
        if (encInBufSize < aencParams.minInBufSize)
            encInBufSize = aencParams.minInBufSize;

        encOutBufSize = MAX_OUTPUT_BUFFER;
        if (encOutBufSize < aencParams.minOutBufSize)
            encOutBufSize = aencParams.minOutBufSize;
    }
    else
    {
        encInBufSize = MAX_INPUT_BUFFER;
        encOutBufSize = MAX_OUTPUT_BUFFER;
    }
    encInBytes = encInBufSize;

    encinBuf = App_allocBuf(encInBytes, TRUE);
    encoutBuf = App_allocBuf(encOutBufSize, TRUE); 
    /* init encoder end */ 


    /* dec */
    memset(&decPrm, 0, sizeof(decPrm));
    memset(&adecParams, 0, sizeof(adecParams));
    adecParams.decoderType = (info->decodeType == 0) ? AUDIO_CODEC_TYPE_AAC_LC : AUDIO_CODEC_TYPE_G711;
    adecParams.desiredChannelMode = info->numChannels;
    if (adecParams.decoderType == AUDIO_CODEC_TYPE_AAC_LC)
        adecParams.desiredChannelMode -= 1;

    printf("To craete acodec - %d!\n", channelIndex);
    decHandle = Adec_create(&adecParams);
    if (decHandle)
    {
        printf ("ADEC Create done...........\n");
    }
    else
    {
        printf ("ADEC Create failed...........\n");
        printf ("\n********Decode APP Task Exitting....\n");
        gAppDecodeThreadActive[channelIndex] = FALSE;
        gAppDecodeThreadExitFlag[channelIndex] = TRUE;
        return NULL;
    }
    gAppDecodeThreadExitFlag[channelIndex] = FALSE;

    
    

    if (adecParams.decoderType == AUDIO_CODEC_TYPE_AAC_LC)
        isSharedRegion = TRUE;

    inBufSize = MAX_INPUT_BUFFER / 2;
    if (inBufSize < adecParams.minInBufSize)
        inBufSize = adecParams.minInBufSize;

    outBufSize = MAX_OUTPUT_BUFFER;
    if (outBufSize < adecParams.minOutBufSize)
        outBufSize = adecParams.minOutBufSize;

    in = fopen(info->inFile, "rb");
    if (!in)
    {
        printf ("File <%s> Open Error....\n", info->inFile);
    }  

    out = fopen(info->outFile, "wb");    
    if (!out)
    {
        printf ("File <%s> Open Error....\n", info->outFile);
    }  

    encOut = fopen(info->outEncFile, "wb");    
    if (!encOut)
    {
        printf ("File <%s> Open Error....\n", info->outEncFile);
    }  

    inBuf = App_allocBuf(inBufSize, isSharedRegion);
    outBuf = App_allocBuf(outBufSize, isSharedRegion); 

    if (in && out && inBuf && outBuf)
    {
        printf ("\n\n=============== Starting Decode (%d) ===================\n", channelIndex);
        sleep(1);

        rdIdx = 0;
        to_read = inBufSize;
        decPrm.outBuf.dataBuf = outBuf;

        while (gAppDecodeThreadActive[channelIndex] == TRUE)
        {
            readBytes = fread (inBuf + rdIdx, 1, to_read, in);
            readBytes += rdIdx;
            if (readBytes)
            {
                decPrm.inBuf.dataBufSize = readBytes;
                decPrm.inBuf.dataBuf = inBuf;
                decPrm.outBuf.dataBufSize = outBufSize;
                if (Adec_process(decHandle, &decPrm) < 0)
                {
                    printf ("DEC: Decode process failed... Corrupted handled!!!! ....\n");
                    break;
                }
                if (decPrm.inBuf.dataBufSize <= 0)
                {
                    if (totalSamples <= 0)
                        printf ("DEC: Decoder didnt consume bytes <%d>... exiting....\n", readBytes);
                    printf ("=============== Decode completed, %d samples generated ================\n", totalSamples);
                    break;
                }

                if ((totalSamples == 0) && (adecParams.decoderType == AUDIO_CODEC_TYPE_AAC_LC))
                {
                    printDecodeStreamParams(&decPrm);
                }

                if (out)
                {
                    writeToPCMFile(out, &decPrm);
                }

                // enc
                if(encOut)
                {
                    encPrm.inBuf.dataBufSize = decPrm.bytesPerSample * (decPrm.numSamples * 2);
                    encPrm.inBuf.dataBuf = decPrm.outBuf.dataBuf;
                    encPrm.outBuf.dataBufSize = encOutBufSize;
                    encPrm.outBuf.dataBuf = encoutBuf;
                    if (Aenc_process(encHandle, &encPrm) < 0)
                    {
                        printf ("ENC: Encode process failed... Corrupted handled!!!! ....\n");
                        break;
                    }
                    else
                    {
                        if (encPrm.outBuf.dataBufSize > 0)
                        {
                            //printf ("ENC: decPrm.bytesPerSample(%d) decPrm.numSamples (%d)encPrm.inBuf.dataBufSize  (%d)!!!! ....\n", 
                            //         decPrm.bytesPerSample,   decPrm.numSamples,   encPrm.inBuf.dataBufSize);
                            //printf ("ENC: Encode process successfuly (%d)!!!! ....\n", encPrm.outBuf.dataBufSize);

                            fwrite(encPrm.outBuf.dataBuf, 1, encPrm.outBuf.dataBufSize, encOut);
                        }
                    }
                }
                
                rdIdx = (readBytes-decPrm.inBuf.dataBufSize);
                memmove(inBuf, inBuf+decPrm.inBuf.dataBufSize, rdIdx);
                to_read = decPrm.inBuf.dataBufSize;
                {
                    //                    printf ("DEC - %d, Samples Generated - %d <total %d>, bytesConsumed - %d\n", 
                    //                            frameCnt, decPrm.numSamples, totalSamples, decPrm.inBuf.dataBufSize);              
                }
                frameCnt++;
                totalSamples += decPrm.numSamples;
            }
            else
            {
                printf ("=============== Decode completed, %d samples generated ================\n", totalSamples);
                break;
            }
        }
    }
    else
    {
        printf ("\n\n=============== Decode not starting.... file / mem error ============\n");
    }
    prm = prm;
    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    if (in)
        fclose(in);
    

    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    if (out)
        fclose(out);

    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    if (encOut)
        fclose(encOut);

    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    if (inBuf)
        App_freeBuf(inBuf, inBufSize, isSharedRegion);

    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    fflush(stdout);
    if (outBuf)
        App_freeBuf(outBuf, outBufSize, isSharedRegion);

    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    if (encinBuf)
        App_freeBuf(encinBuf, encInBytes, isSharedRegion);

    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    if (encoutBuf)
        App_freeBuf(encoutBuf, encOutBufSize, isSharedRegion);

    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    Aenc_delete(encHandle);

    printf("%s %d\n", __func__, __LINE__);
    fflush(stdout);
    Adec_delete(decHandle);

    printf ("\n\n=============== Exiting Decode (%d) ===================\n", channelIndex);
    gAppDecodeThreadActive[channelIndex] = FALSE;
    gAppDecodeThreadExitFlag[channelIndex] = TRUE;
    return NULL;
}

Bool startAudioDecodeSystem (Void)
{
    Bool ret = FALSE;
    Int32 status;
    Int32 index = 0;
    Int32 transcodeNum = 0;
    Audio_DecInfo stAuidoInfo[WV_MAX_AUDIO_NUM];

    #if 0
    printf("\r\nAUDIO: Enter input file name <absolute path>: " );
    fflush(stdin);
    fgets(gDecInfo.inFile, MAX_INPUT_STR_SIZE, stdin);
    gDecInfo.inFile[ strlen(gDecInfo.inFile)-1 ] = 0;

    printf("\r\nAUDIO: Enter output file name <absolute path>: " );
    fflush(stdin);
    fgets(gDecInfo.outFile, MAX_INPUT_STR_SIZE, stdin);
    gDecInfo.outFile[ strlen(gDecInfo.outFile)-1 ] = 0;

    transcodeNum = getIntValue("AUDIO: channel number <1 ~ 12 >", 1, 12, 0);
    

    gDecInfo.decodeType = getIntValue("AUDIO: decode Type <0 - AAC-LC, 1 - G711>", 0, 1, 0);
    if (gDecInfo.decodeType == 0)
    {
        gDecInfo.numChannels = getIntValue("AUDIO: desired audio channels", 1, 2, 1);
    }
    #endif
    transcodeNum = getIntValue("AUDIO: channel number <1 ~ 12 >", 1, 12, 0);
    sprintf(gDecInfo.inFile, "%s", "/opt/dvr_rdk/ti816x/bin/multich_audio/song.aac");
    sprintf(gDecInfo.outFile, "%s", "/opt/dvr_rdk/ti816x/bin/multich_audio/song_pcm");
    sprintf(gDecInfo.outEncFile, "%s", "/opt/dvr_rdk/ti816x/bin/multich_audio/song_enc");
    gDecInfo.numChannels = 2;
    gDecInfo.decodeType = 0;
    
    time_t startTime; 
    startTime = time(NULL);
    
    for( index = 0; index < transcodeNum; index++)
    {  
        gAppDecodeThreadActive[index] = TRUE;
    
        sprintf(stAuidoInfo[index].inFile, "%s", gDecInfo.inFile);
        sprintf(stAuidoInfo[index].outFile, "%s-%d.pcm", gDecInfo.outFile, index);
        sprintf(stAuidoInfo[index].outEncFile, "%s-%d.aac", gDecInfo.outEncFile, index);
        stAuidoInfo[index].numChannels = gDecInfo.numChannels;
        stAuidoInfo[index].decodeType = gDecInfo.decodeType;
        stAuidoInfo[index].channelIndex= index;
    
        status = OSA_thrCreate(&gAppDecodeThread[index],
                App_decodeTaskFxn,
                AUDIO_TSK_PRI, 
                AUDIO_TSK_STACK_SIZE, 
                &stAuidoInfo[index]);
        if (status != 0)
        {
            printf ("AUDIO: App Decode thread create failed...\n");
            gAppDecodeThreadActive[index] = FALSE;
            return ret;
        }

        usleep(5000);
    }

    printf ("\r\n\n");
    while (1)
    {
        Bool isEnd = TRUE;
        for( index = 0; index < transcodeNum; index++)
        {
            if(gAppDecodeThreadExitFlag[index] == FALSE)
            {
                isEnd = FALSE;
            }
        }

        if(TRUE == isEnd)
            break;
    
        printf ("**** Waiting for Decode task to exit .....\n");
        OSA_waitMsecs(1000);
    }

    time_t endTime; 
    endTime = time(NULL);

    printf("Total - %d \n", (int)(endTime - startTime));

    usleep(100000);
    for( index = 0; index < transcodeNum; index++)
    {
        OSA_thrDelete(&gAppDecodeThread[index]);
    }
    printf ("AUDIO:  Decode stopped....\n");

    return TRUE;
}

Void *App_allocBuf(Int32 bufSize, Bool fromSharedRegion)
{
    if (fromSharedRegion == TRUE)
        return Audio_allocateSharedRegionBuf(bufSize);
    else
        return malloc(bufSize);
}

Void App_freeBuf (Void *buf, Int32 bufSize, Bool fromSharedRegion)
{
    if (fromSharedRegion == TRUE)
        Audio_freeSharedRegionBuf(buf, bufSize);
    else
        free(buf);
}

char getChar()
{
    char buffer[MAX_INPUT_STR_SIZE];

    fflush(stdin);
    fgets(buffer, MAX_INPUT_STR_SIZE, stdin);

    return(buffer[0]);
}

int getIntValue(char *string, int minVal, int maxVal, int defaultVal)
{
    char inputStr[MAX_INPUT_STR_SIZE];
    int value;

    printf(" \n");
    printf(" Enter %s [Valid values, %d .. %d] : ", string, minVal, maxVal);

    fflush(stdin);
    fgets(inputStr, MAX_INPUT_STR_SIZE, stdin);

    value = atoi(inputStr);

    if(value < minVal || value > maxVal )
    {
        value = defaultVal;
        printf(" \n");
        printf("WARNING: Invalid value specified, defaulting to value of = %d \n", value);
    }
    else
    {
        printf(" \n");
        printf(" Entered value = %d \n", value);
    }

    printf(" \n");

    return value;
}


