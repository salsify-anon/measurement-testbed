/* -LICENSE-START-
** Copyright (c) 2013 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <mutex>
#include <queue>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iostream>

#include "DeckLinkAPI.h"
#include "Capture.hh"
#include "Config.hh"
#include "display.hh"
#include "chunk.hh"
#include "barcode.hh"

using std::chrono::time_point;
using std::chrono::high_resolution_clock;
using std::chrono::time_point_cast;
using std::chrono::microseconds;

std::queue<IDeckLinkVideoInputFrame*> frame_queue;
std::mutex frame_queue_lock;

 const BMDTimeScale ticks_per_second = (BMDTimeScale)1000000; /* microsecond resolution */ 
static BMDTimeScale prev_frame_recieved_time = (BMDTimeScale)0;

static pthread_mutex_t  g_sleepMutex;
static pthread_cond_t   g_sleepCond;
static int              g_videoOutputFile = -1;
static bool             g_do_exit = false;

static BMDConfig        g_config;

static IDeckLinkInput*  g_deckLinkInput = NULL;
static std::ofstream    logfile;

static int64_t  g_frameCount = 0;
static uint64_t g_validFrameCount = 0;

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate() : 
    m_refCount(1)
{
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
    return __sync_add_and_fetch(&m_refCount, 1);
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
    int32_t newRefValue = __sync_sub_and_fetch(&m_refCount, 1);
    if (newRefValue == 0)
    {
        delete this;
        return 0;
    }
    return newRefValue;
}

void DeckLinkCaptureDelegate::preview(void*, int) {}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket*)
{
    void*                               frameBytes;

    /* IMPORTANT: get the frame arrived timestamp */
    time_point<high_resolution_clock> tp = high_resolution_clock::now();
    
    BMDTimeValue decklink_hardware_timestamp;
    BMDTimeValue decklink_time_in_frame;
    BMDTimeValue decklink_ticks_per_frame;
    HRESULT ret;
    ret = g_deckLinkInput->GetHardwareReferenceClock(ticks_per_second, 
                                                     &decklink_hardware_timestamp,
                                                     &decklink_time_in_frame,
                                                     &decklink_ticks_per_frame);
            
    BMDTimeValue decklink_frame_reference_timestamp;
    BMDTimeValue decklink_frame_reference_duration;
    if( (ret = videoFrame->GetHardwareReferenceTimestamp(ticks_per_second,
                                                         &decklink_frame_reference_timestamp,
                                                         &decklink_frame_reference_duration) ) != S_OK ) {
        
        std::cerr << "GetHardwareReferenceTimestamp: could not get HardwareReferenceTimestamp for frame timestamp" << std::endl;
        return ret;
    }
    
    if( prev_frame_recieved_time == 0 ){
        prev_frame_recieved_time = decklink_frame_reference_timestamp;
    }
    else if( decklink_frame_reference_timestamp - prev_frame_recieved_time > 20000 ){
        std::cerr << "Frame was late! Delay was: " << decklink_frame_reference_timestamp - prev_frame_recieved_time << std::endl;
        throw std::runtime_error("Capture was LATE when capturing a frame.\n");            
    }
    else{
        prev_frame_recieved_time = decklink_frame_reference_timestamp;
    } 
  
    // Handle Video Frame
    if (videoFrame)
    {
        // If 3D mode is enabled we retreive the 3D extensions interface which gives.
        // us access to the right eye frame by calling GetFrameForRightEye() .

        if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
        {
            printf("Frame received (#%lu) - No input signal detected\n", g_frameCount);
        }
        else
        {
            uint64_t framesize = videoFrame->GetRowBytes() * videoFrame->GetHeight();

            videoFrame->GetBytes(&frameBytes);
            Chunk chunk((uint8_t*)frameBytes, framesize);
            XImage img(chunk, videoFrame->GetWidth(), videoFrame->GetHeight());
            auto barcodes = Barcode::readBarcodes(img);
            
            printf("Frame received (#%lu) - %s - Size: %li bytes\n",
                   g_frameCount,
                   "Valid Frame",
                   framesize);

            /* IMPORTANT: log the timestamps  */
            if (barcodes.first != 0xFFFFFFFFFFFFFFFF 
                || barcodes.second != 0xFFFFFFFFFFFFFFFF) {
                if (logfile.is_open())
                    logfile << g_validFrameCount << "," 
                            << barcodes.first << "," << barcodes.second << ","
                            << time_point_cast<microseconds>(tp).time_since_epoch().count() << ","
                            << decklink_hardware_timestamp << ","
                            << decklink_frame_reference_timestamp << ","
                            << decklink_frame_reference_duration 
                            << std::endl;
                else
                    std::cout   << g_validFrameCount << "," 
                                << barcodes.first << "," << barcodes.second << ","
                                << time_point_cast<microseconds>(tp).time_since_epoch().count() << ","
                                << decklink_hardware_timestamp << ","
                                << decklink_frame_reference_timestamp << ","
                                << decklink_frame_reference_duration 
                                << std::endl;
                g_validFrameCount++;

                if (g_videoOutputFile != -1)
                {
                    std::lock_guard<std::mutex> lg(frame_queue_lock);
                    videoFrame->AddRef();
                    frame_queue.push(videoFrame);
                    // ssize_t ret = write(g_videoOutputFile, frameBytes, framesize);
                    // if (ret < 0) 
                    //     fprintf(stderr, "Cannot write to file.\n");
                }
            }


            preview(frameBytes, videoFrame->GetRowBytes() * videoFrame->GetHeight());
        }

        g_frameCount++;
    }

    if (g_config.m_maxFrames > 0 && videoFrame && g_frameCount >= g_config.m_maxFrames)
    {
        g_do_exit = true;
        pthread_cond_signal(&g_sleepCond);
    }

    return S_OK;
}


HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags formatFlags)
{
    // This only gets called if bmdVideoInputEnableFormatDetection was set
    // when enabling video input
    HRESULT result;
    char*   displayModeName = NULL;
    BMDPixelFormat  pixelFormat = bmdFormat10BitYUV;

    if (formatFlags & bmdDetectedVideoInputRGB444)
        pixelFormat = bmdFormat10BitRGB;

    mode->GetName((const char**)&displayModeName);
    printf("Video format changed to %s %s\n", displayModeName, formatFlags & bmdDetectedVideoInputRGB444 ? "RGB" : "YUV");

    if (displayModeName)
        free(displayModeName);

    if (g_deckLinkInput)
    {
        g_deckLinkInput->StopStreams();

        result = g_deckLinkInput->EnableVideoInput(mode->GetDisplayMode(), pixelFormat, g_config.m_inputFlags);
        if (result != S_OK)
        {
            fprintf(stderr, "Failed to switch video mode\n");
            goto bail;
        }

        g_deckLinkInput->StartStreams();
    }

bail:
    return S_OK;
}

class DeckLinkCapturePreview : public DeckLinkCaptureDelegate 
{
public:
    DeckLinkCapturePreview(int width, int height) : 
        DeckLinkCaptureDelegate(),
        window(width, height),
        picture(window),
        image(picture),
        gc(picture) 
    {
        window.set_name( "Capture Preview" );
        window.map();
    }

    virtual void preview(void* frame_bytes, int frame_size) {
        memcpy(image.data_unsafe(), frame_bytes, frame_size);
        picture.put( image, gc );
        window.present( picture, 0, 0 );
    }

private:
    XWindow             window;
    XPixmap             picture;
    XImage              image;
    GraphicsContext     gc;

};


static void sigfunc(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
        g_do_exit = true;

    pthread_cond_signal(&g_sleepCond);
}

int main(int argc, char *argv[])
{
    HRESULT                         result;
    int                             exitStatus = 1;
    int                             idx;

    IDeckLinkIterator*              deckLinkIterator = NULL;
    IDeckLink*                      deckLink = NULL;

    IDeckLinkAttributes*            deckLinkAttributes = NULL;
    bool                            formatDetectionSupported;

    IDeckLinkDisplayModeIterator*   displayModeIterator = NULL;
    IDeckLinkDisplayMode*           displayMode = NULL;
    char*                           displayModeName = NULL;
    BMDDisplayModeSupport           displayModeSupported;

    DeckLinkCaptureDelegate*        delegate = NULL;

    pthread_mutex_init(&g_sleepMutex, NULL);
    pthread_cond_init(&g_sleepCond, NULL);

    signal(SIGINT, sigfunc);
    signal(SIGTERM, sigfunc);
    signal(SIGHUP, sigfunc);

    // Process the command line arguments
    if (!g_config.ParseArguments(argc, argv))
    {
        g_config.DisplayUsage(exitStatus);
        goto bail;
    }

    // Get the DeckLink device
    deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!deckLinkIterator)
    {
        fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
        goto bail;
    }

    idx = g_config.m_deckLinkIndex;

    while ((result = deckLinkIterator->Next(&deckLink)) == S_OK)
    {
        if (idx == 0)
            break;
        --idx;

        deckLink->Release();
    }

    if (result != S_OK || deckLink == NULL)
    {
        fprintf(stderr, "Unable to get DeckLink device %u\n", g_config.m_deckLinkIndex);
        goto bail;
    }

    // Get the input (capture) interface of the DeckLink device
    result = deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&g_deckLinkInput);
    if (result != S_OK)
        goto bail;

    // Get the display mode
    if (g_config.m_displayModeIndex == -1)
    {
        // Check the card supports format detection
        result = deckLink->QueryInterface(IID_IDeckLinkAttributes, (void**)&deckLinkAttributes);
        if (result == S_OK)
        {
            result = deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &formatDetectionSupported);
            if (result != S_OK || !formatDetectionSupported)
            {
                fprintf(stderr, "Format detection is not supported on this device\n");
                goto bail;
            }
        }

        g_config.m_inputFlags |= bmdVideoInputEnableFormatDetection;

        // Format detection still needs a valid mode to start with
        idx = 0;
    }
    else
    {
        idx = g_config.m_displayModeIndex;
    }

    result = g_deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK)
        goto bail;

    while ((result = displayModeIterator->Next(&displayMode)) == S_OK)
    {
        if (idx == 0)
            break;
        --idx;

        displayMode->Release();
    }

    if (result != S_OK || displayMode == NULL)
    {
        fprintf(stderr, "Unable to get display mode %d\n", g_config.m_displayModeIndex);
        goto bail;
    }

    // Get display mode name
    result = displayMode->GetName((const char**)&displayModeName);
    if (result != S_OK)
    {
        displayModeName = (char *)malloc(32);
        snprintf(displayModeName, 32, "[index %d]", g_config.m_displayModeIndex);
    }

    // Check display mode is supported with given options
    result = g_deckLinkInput->DoesSupportVideoMode(displayMode->GetDisplayMode(), g_config.m_pixelFormat, bmdVideoInputFlagDefault, &displayModeSupported, NULL);
    if (result != S_OK)
        goto bail;

    if (displayModeSupported == bmdDisplayModeNotSupported)
    {
        fprintf(stderr, "The display mode %s is not supported with the selected pixel format\n", displayModeName);
        goto bail;
    }

    // Print the selected configuration
    g_config.DisplayConfiguration();

    // Configure the capture callback
    if (g_config.m_playback)
        delegate = new DeckLinkCapturePreview(displayMode->GetWidth(), displayMode->GetHeight());
    else
        delegate = new DeckLinkCaptureDelegate();
    g_deckLinkInput->SetCallback(delegate);

    // Open output files
    if (g_config.m_videoOutputFile != NULL)
    {
        g_videoOutputFile = open(g_config.m_videoOutputFile, O_WRONLY|O_CREAT|O_TRUNC, 0664);
        if (g_videoOutputFile < 0)
        {
            fprintf(stderr, "Could not open video output file \"%s\"\n", g_config.m_videoOutputFile);
            goto bail;
        }
    }

    if (g_config.m_logFilename != NULL) {
        logfile.open(g_config.m_logFilename, std::ios::out);
        if (!logfile.is_open()) {
            fprintf(stderr, "Error opening logfile.\n");
            goto bail;
        }
        else {
            /* IMPORTANT: write header to csv log file */
            std::time_t time = std::time(nullptr);
            logfile << "# Reading from decklink interface to the video file: " << g_config.m_videoOutputFile << std::endl
                    << "# Time stamp: " << std::asctime(std::localtime(&time))
                    << "# frame_index,upper_left_barcode,lower_right_barcode,cpu_timestamp,decklink_hardwaretimestamp,decklink_frame_reference_timestamp,decklink_frame_reference_duration"
                    << "\n";
        }
    }

    // Block main thread until signal occurs
    while (!g_do_exit)
    {
        // Start capturing
        result = g_deckLinkInput->EnableVideoInput(displayMode->GetDisplayMode(), g_config.m_pixelFormat, g_config.m_inputFlags);
        if (result != S_OK)
        {
            fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
            goto bail;
        }

        result = g_deckLinkInput->StartStreams();
        if (result != S_OK)
            goto bail;
        
        const unsigned int framesize = 1280 * 720 * 4;
        while ( !g_do_exit || !frame_queue.empty()) {
            IDeckLinkVideoInputFrame* frame = nullptr;
            {
                std::lock_guard<std::mutex> lg(frame_queue_lock);
                if( !frame_queue.empty() ) {
                    frame = frame_queue.front();
                    frame_queue.pop();
                }
            }
            if ( frame != nullptr ){
                uint8_t* buffer = nullptr;
                frame->GetBytes((void**)&buffer);

                ssize_t ret = write(g_videoOutputFile, buffer, framesize);
                if (ret < 0) 
                    fprintf(stderr, "Cannot write to file.\n");

                frame->Release();
            }
            else{
                usleep(100);
            }
        }

        // All Okay.
        exitStatus = 0;

        pthread_mutex_lock(&g_sleepMutex);
        pthread_cond_wait(&g_sleepCond, &g_sleepMutex);
        pthread_mutex_unlock(&g_sleepMutex);

        fprintf(stderr, "Stopping Capture\n");
        g_deckLinkInput->StopStreams();
        g_deckLinkInput->DisableVideoInput();
    }

bail:
    if (g_videoOutputFile != 0)
        close(g_videoOutputFile);

    if (displayModeName != NULL)
        free(displayModeName);

    if (displayMode != NULL)
        displayMode->Release();

    if (displayModeIterator != NULL)
        displayModeIterator->Release();

    if (delegate != NULL)
        delegate->Release();

    if (g_deckLinkInput != NULL)
    {
        g_deckLinkInput->Release();
        g_deckLinkInput = NULL;
    }

    if (deckLinkAttributes != NULL)
        deckLinkAttributes->Release();

    if (deckLink != NULL)
        deckLink->Release();

    if (deckLinkIterator != NULL)
        deckLinkIterator->Release();

    if (logfile.is_open())
        logfile.close();

    return exitStatus;
}
