/********************************************************************************/
/*                                                                              */
/* Copyright (c) Microsoft Corporation. All rights reserved.					*/
/* Portions Copyright(c) 2020 Analog Devices Inc.								*/
/* Licensed under the MIT License.												*/
/*																				*/
/********************************************************************************/

#include "ADIView.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <iostream>
//#include <GL\gl3w.h>
#include <math.h>

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to
// maximize ease of testing and compatibility with old VS compilers. To link
// with VS2010-era libraries, VS2015+ requires linking with
// legacy_stdio_definitions.lib, which we do using this pragma. Your own project
// should not be affected, as you are likely to link with a newer binary of GLFW
// that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) &&                                 \
    !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

#undef min
#undef max

using namespace adiviewer;
using namespace adicontroller;

ADIView::ADIView(std::shared_ptr<ADIController> ctrl, const std::string &name)
    : m_ctrl(ctrl), m_viewName(name), m_depthFrameAvailable(false),
      m_center(true), m_waitKeyBarrier(0), m_distanceVal(0),
      m_smallSignal(false), m_crtSmallSignalState(false) {
    //Create AB and Depth independent threads
    m_depthImageWorker =
        std::thread(std::bind(&ADIView::_displayDepthImage, this));
    m_abImageWorker = std::thread(std::bind(&ADIView::_displayAbImage, this));

    //Create a Point Cloud independent thread
    m_pointCloudImageWorker =
        std::thread(std::bind(&ADIView::_displayPointCloudImage, this));
}

ADIView::~ADIView() {
    if (m_ctrl->hasCamera()) {
        m_ctrl->StopCapture();
    }

    std::unique_lock<std::mutex> lock(m_frameCapturedMutex);
    m_stopWorkersFlag = true;
    lock.unlock();
    m_frameCapturedCv.notify_all();
    m_depthImageWorker.join();
    m_abImageWorker.join();
    m_pointCloudImageWorker.join();
}

void ADIView::showWindows() {}

static void glfw_error_callback(int error, const char *description) {
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

void ADIView::captureVideo() {}

void ADIView::initVideo() {}

uint8_t *ADIView::imageRender(uint16_t *image) {
    uint8_t *processedImage = nullptr;
    return processedImage;
}

void ADIView::initShaders() {}

void ADIView::render() {
    // Our state
    bool show_demo_window = true;
    bool show_another_window = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    //Camera booleans
    bool captureEnabled = false;
    bool captureBlendedEnabled = false;
    bool recordEnabled = false;
    bool playbackEnabled = false;

    //Camera Strings
    std::string status = "";

    std::string modes[3] = {"near", "medium", "far"};

    // Main imGUI loop
    while (!glfwWindowShouldClose(window)) {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to
        // tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data
        // to your main application.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input
        // data to your main application. Generally you may always pass all
        // inputs to dear imgui, and hide them from your application based on
        // those two flags.
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Menu Bar
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Imager Options")) {
                ImGui::MenuItem("Show AB Window", NULL, &showABWindow);
                ImGui::MenuItem("Show Depth Window", NULL, &showDepthWindow);
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }

        // Show AB Window
        if (showABWindow) {
            ImGui::Begin("AB Window", &showABWindow);
            ImGui::Text("Display AB View");
            if (ImGui::Button("Close AB Window")) {
                showABWindow = false;
            }
            captureEnabled =
                ImGui::Checkbox("Display AB Image", &beginDisplayABImage);
            if (beginDisplayABImage) {
                //Do some work here
                if (m_ctrl->hasCamera()) {
                    if (captureEnabled) {
                        // TODO: set camera mode here
                        int selectedMode = 0;

                        m_ctrl->StartCapture();
                        m_ctrl->requestFrame();
                        status = "Playing from device!";
                        initShaders();
                    } else {
                        m_capturedFrame = m_ctrl->getFrame();

                        std::unique_lock<std::mutex> lock(m_frameCapturedMutex);
                        m_depthFrameAvailable = true;
                        m_abFrameAvailable = true;
                        lock.unlock();
                        m_frameCapturedCv.notify_all();
                        m_ctrl->requestFrame();
                        //if (needsInit)
                        {
                            needsInit = false;
                            captureVideo();
                        }
                        const ImVec2 sourceImageDimensions = {
                            static_cast<float>(640), static_cast<float>(480)};

                        /*********************************************/
                        glBindTexture(GL_TEXTURE_2D, video_texture);
                        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
                        /***********************************************/
                        ImGui::Image((void *)(intptr_t)video_texture,
                                     sourceImageDimensions);
                    }
                } else {
                    status = "No cameras connected!";
                    if (needsInit) {
                        initVideo();
                        needsInit = false;
                    }
                    ImGui::Image((void *)(intptr_t)video_texture,
                                 ImVec2(64, 64));
                }
            } else {
                if (m_ctrl->hasCamera()) {
                    if (captureEnabled) {
                        m_ctrl->StopCapture();
                    }
                }
            }

            ImGui::Checkbox("Display Depth Image", &beginDisplayDepthImage);
            if (beginDisplayDepthImage) {

                aditof::Status status;

                // Initialize first TOF camera
                std::shared_ptr<aditof::Camera> camera1 =
                    m_ctrl->m_cameras.front();
                status = camera1->initialize();

                // Choose the mode the camera should produce
                std::vector<uint8_t> availableModes;
                camera1->getAvailableModes(availableModes);
                status = camera1->setMode(availableModes.front());

                // Place the camera in a specific mode
                std::vector<std::string> modes;
                camera1->getAvailableControls(modes);

                // Create a TOF frame and request data from the TOF camera
                aditof::Frame frame;

                status = camera1->start();
                status = camera1->requestFrame(&frame);
                uint16_t *data;
                frame.getData("depth", &data);

                aditof::FrameDataDetails frameDepthDetails;
                status = frame.getDataDetails("depth", frameDepthDetails);
                int frameWidth = frameDepthDetails.width;
                int frameHeight = frameDepthDetails.height;

                /*status = camera1->requestFrame(&frame);*/
                // Create a OpenGL texture identifier
                GLuint image_texture;
                glGenTextures(1, &image_texture);
                glBindTexture(GL_TEXTURE_2D, image_texture);
                // Setup filtering parameters for display
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                GL_LINEAR);

                // Upload pixels into texture
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

                const ImVec2 sourceImageDimensions = {
                    static_cast<float>(640.0), static_cast<float>(960.0)};
                ImGui::Image((void *)(intptr_t)image_texture,
                             sourceImageDimensions);
            }

            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        /*glClearColor(clear_color.x, clear_color.y, clear_color.z,
					 clear_color.w);*/
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }
}

void ADIView::setABMaxRange(std::string value) {
    uint16_t base = 13; // Cap at 8191

    if (getCapABWidth() == false) {
        if (value == "6")
            base = 16;
        else if (value == "5")
            base = 14;
        else if (value == "4")
            base = 12;
        else if (value == "3")
            base = 10;
        else
            base = 8;
    }
    m_maxABPixelValue = (1 << base) - 1;
}

static void normalizeABBuffer(uint16_t *abBuffer, uint16_t abWidth,
                              uint16_t abHeight, bool advanceScaling,
                              bool useLogScaling) {

    size_t imageSize = abHeight * abWidth;

    uint32_t min_value_of_AB_pixel = 0xFFFF;
    uint32_t max_value_of_AB_pixel = 1;

    if (advanceScaling) {

        for (size_t dummyCtr = 0; dummyCtr < imageSize; ++dummyCtr) {
            if (abBuffer[dummyCtr] > max_value_of_AB_pixel) {
                max_value_of_AB_pixel = abBuffer[dummyCtr];
            }
            if (abBuffer[dummyCtr] < min_value_of_AB_pixel) {
                min_value_of_AB_pixel = abBuffer[dummyCtr];
            }
        }
        max_value_of_AB_pixel -= min_value_of_AB_pixel;
    } else {

        //TODO: This is hard code, but should reflect the number of AB bits
        //      See https://github.com/analogdevicesinc/ToF/blob/7d63e2d7e0e2bb795f5a55139740b4d5870ad3d6/examples/tof-viewer/src/ADIView.cpp#L254
        uint32_t m_maxABPixelValue = (1 << 13) - 1;
        max_value_of_AB_pixel = m_maxABPixelValue;
        min_value_of_AB_pixel = 0;
    }

    uint32_t new_max_value_of_AB_pixel = 1;
    uint32_t new_min_value_of_AB_pixel = 0xFFFF;

#pragma omp parallel for
    for (size_t dummyCtr = 0; dummyCtr < imageSize; ++dummyCtr) {

        abBuffer[dummyCtr] -= min_value_of_AB_pixel;
        double pix = abBuffer[dummyCtr] * (255.0 / max_value_of_AB_pixel);

        if (pix < 0.0) {
            pix = 0.0;
        }
        if (pix > 255.0) {
            pix = 255.0;
        }
        abBuffer[dummyCtr] = static_cast<uint8_t>(pix);

        if (abBuffer[dummyCtr] > new_max_value_of_AB_pixel) {
            new_max_value_of_AB_pixel = abBuffer[dummyCtr];
        }
        if (abBuffer[dummyCtr] < new_min_value_of_AB_pixel) {
            new_min_value_of_AB_pixel = abBuffer[dummyCtr];
        }
    }

    if (useLogScaling) {

        max_value_of_AB_pixel = new_max_value_of_AB_pixel;
        min_value_of_AB_pixel = new_min_value_of_AB_pixel;

        double maxLogVal =
            log10(1.0 + static_cast<double>(max_value_of_AB_pixel -
                                            min_value_of_AB_pixel));

#pragma omp parallel for
        for (size_t dummyCtr = 0; dummyCtr < imageSize; ++dummyCtr) {

            double pix =
                static_cast<double>(abBuffer[dummyCtr] - min_value_of_AB_pixel);
            double logPix = log10(1.0 + pix);
            pix = (logPix / maxLogVal) * 255.0;
            if (pix < 0.0) {
                pix = 0.0;
            }
            if (pix > 255.0) {
                pix = 255.0;
            }

            abBuffer[dummyCtr] = (uint8_t)(pix);
        }
    }
}

void ADIView::_displayAbImage() {
    while (!m_stopWorkersFlag) {
        std::unique_lock<std::mutex> lock(m_frameCapturedMutex);
        m_frameCapturedCv.wait(
            lock, [&]() { return m_abFrameAvailable || m_stopWorkersFlag; });

        if (m_stopWorkersFlag) {
            break;
        }

        m_abFrameAvailable = false;
        if (m_capturedFrame == nullptr) {
            continue;
        }

        lock.unlock(); // Lock is no longer needed

        m_capturedFrame->getData("ab", &ab_video_data);

        aditof::FrameDataDetails frameAbDetails;
        frameAbDetails.height = 0;
        frameAbDetails.width = 0;
        m_capturedFrame->getDataDetails("ab", frameAbDetails);

        frameHeight = static_cast<int>(frameAbDetails.height);
        frameWidth = static_cast<int>(frameAbDetails.width);

        // Update a copy of the AB frame buffer since the origina may be used by the recorder.
        uint16_t *_ab_video_data = new uint16_t[frameHeight * frameWidth];

        if (_ab_video_data == nullptr) {
            return;
        }

        memcpy(_ab_video_data, ab_video_data,
               frameHeight * frameWidth * sizeof(uint16_t));

        normalizeABBuffer(_ab_video_data, frameWidth, frameHeight,
                          getAutoScale(), getLogImage());

        size_t imageSize = frameHeight * frameWidth;
        size_t bgrSize = 0;
        ab_video_data_8bit = new uint8_t[frameHeight * frameWidth * 3];

        for (size_t dummyCtr = 0; dummyCtr < imageSize; dummyCtr++) {
            uint16_t pix = _ab_video_data[dummyCtr];
            ab_video_data_8bit[bgrSize++] = (uint8_t)(pix);
            ab_video_data_8bit[bgrSize++] = (uint8_t)(pix);
            ab_video_data_8bit[bgrSize++] = (uint8_t)(pix);
        }

        // Create a OpenGL texture identifier
        if (needsInit) {
            needsInit = false;
        }

        std::unique_lock<std::mutex> imshow_lock(m_imshowMutex);
        m_waitKeyBarrier += 1;
        if (m_waitKeyBarrier == /*2*/ numOfThreads) {
            imshow_lock.unlock();
            m_barrierCv.notify_one();
        }

        delete[] _ab_video_data;
    }
}
static std::chrono::duration<double, std::milli> millisecondsPast;
static std::chrono::high_resolution_clock::time_point t1;
static std::chrono::high_resolution_clock::time_point t2;
static std::chrono::duration<double, std::milli> time_span;
static std::chrono::duration<double, std::milli> averagemilliSecs;

void ADIView::_displayDepthImage() {
    while (!m_stopWorkersFlag) {
        std::unique_lock<std::mutex> lock(m_frameCapturedMutex);
        m_frameCapturedCv.wait(
            lock, [&]() { return m_depthFrameAvailable || m_stopWorkersFlag; });

        if (m_stopWorkersFlag) {
            break;
        }

        m_depthFrameAvailable = false;

        if (m_capturedFrame == nullptr) {
            continue;
        }

        lock.unlock(); // Lock is no longer needed

        //number of frames
        //CPU process power/cycles?
        //Capture time
        t1 = std::chrono::high_resolution_clock::now();

        uint16_t *data;
        m_capturedFrame->getData("depth", &depth_video_data);
        t2 = std::chrono::high_resolution_clock::now();

        time_span = t2 - t1; //Delayed process time
        averagemilliSecs =
            (time_span + millisecondsPast) / 2; //average time to get Depth data
        millisecondsPast = time_span;
        aditof::FrameDataDetails frameDepthDetails;
        m_capturedFrame->getDataDetails("depth", frameDepthDetails);

        frameHeight = static_cast<int>(frameDepthDetails.height);
        frameWidth = static_cast<int>(frameDepthDetails.width);

        constexpr uint8_t PixelMax = std::numeric_limits<uint8_t>::max();
        size_t imageSize = frameHeight * frameWidth;
        size_t bgrSize = 0;
        depth_video_data_8bit =
            new uint8_t[frameHeight * frameWidth * 3]; //Multiplied by BGR

        //CCD implementation works with the below parameters
        /*int max = m_ctrl->getRangeMax();
		int min = m_ctrl->getRangeMin();*/

        //TODO: Implement temp display
#if 0
        std::string attrVal;
        uint8_t totalcaptures = static_cast<uint8_t>(std::stoi(attrVal));


		// TODO: get proper equation per module
		//temperature_c = subframeHeader[aditof::TEMP_SENSOR_ADC];
		//temperature_c = (temperature_c - 1493.55) * 5.45;
#endif

        float fRed = 0.f;
        float fGreen = 0.f;
        float fBlue = 0.f;
        uint8_t blueErase;
        uint8_t greenErase;
        uint8_t redErase;

        //Create a dummy for loop that mimics some future process
        for (size_t dummyCtr = 0; dummyCtr < imageSize; dummyCtr++) {
            //It is doing a width x height x 3: Resolution * 3bytes (BGR)
            if (depth_video_data[dummyCtr] ==
                0) { //If pixel is actual zero, leave it as zero
                depth_video_data_8bit[bgrSize++] = 0;
                depth_video_data_8bit[bgrSize++] = 0;
                depth_video_data_8bit[bgrSize++] = 0;
            } else {
                hsvColorMap(depth_video_data[dummyCtr], maxRange, minRange,
                            fRed, fGreen, fBlue);
                depth_video_data_8bit[bgrSize++] =
                    static_cast<uint8_t>(fBlue * PixelMax); //Blue
                depth_video_data_8bit[bgrSize++] =
                    static_cast<uint8_t>(fGreen * PixelMax); //Green
                depth_video_data_8bit[bgrSize++] =
                    static_cast<uint8_t>(fRed * PixelMax); //Red

                blueErase = static_cast<uint8_t>(fBlue * PixelMax);
                greenErase = static_cast<uint8_t>(fGreen * PixelMax);
                redErase = static_cast<uint8_t>(fRed * PixelMax);
            }
        }

        std::unique_lock<std::mutex> imshow_lock(m_imshowMutex);
        m_waitKeyBarrier += 1;
        if (m_waitKeyBarrier == /*2*/ numOfThreads) {
            imshow_lock.unlock();
            m_barrierCv.notify_one();
        }
    }
}

void ADIView::_displayBlendedImage() {
    std::shared_ptr<aditof::Frame> localFrame = m_capturedFrame;

    uint16_t *abData;
    localFrame->getData("ab", &abData);
}

void ADIView::_displayPointCloudImage() {
    while (!m_stopWorkersFlag) {
        std::unique_lock<std::mutex> lock(m_frameCapturedMutex);
        m_frameCapturedCv.wait(lock, [&]() {
            return m_pointCloudFrameAvailable || m_stopWorkersFlag;
        });

        if (m_stopWorkersFlag) {
            break;
        }

        m_pointCloudFrameAvailable = false;
        if (m_capturedFrame == nullptr) {
            continue;
        }

        lock.unlock(); // Lock is no longer needed

        //Get XYZ table
        m_capturedFrame->getData("xyz", &pointCloud_video_data);
        aditof::FrameDataDetails frameXyzDetails;
        frameXyzDetails.height = 0;
        frameXyzDetails.width = 0;
        m_capturedFrame->getDataDetails("xyz", frameXyzDetails);
        frameHeight = static_cast<int>(frameXyzDetails.height);
        frameWidth = static_cast<int>(frameXyzDetails.width);

        //Size is [XX, YY, ZZ] x Width x Height
        size_t frameSize = frameHeight * frameWidth * 3;
        if (pointcloudTableSize != frameSize) {
            if (normalized_vertices) {
                delete[] normalized_vertices;
            }
            pointcloudTableSize = frameSize;
            normalized_vertices =
                new float[pointcloudTableSize * 3]; //Adding RGB components
        }

        float fRed = 0.f;
        float fGreen = 0.f;
        float fBlue = 0.f;
        size_t bgrSize = 0;

        constexpr uint8_t PixelMax = std::numeric_limits<uint8_t>::max();

        //1) convert the buffer from uint16 to float
        //2) normalize between [-1.0, 1.0]
        //3) X and Y ranges between [-32768, 32767] or [FFFF, 7FFF]. Z axis is [0, 7FFF]

        for (int i = 0; i < pointcloudTableSize; i += 3) {
            normalized_vertices[bgrSize++] =
                (((int16_t)pointCloud_video_data[i])) / (Max_X);
            normalized_vertices[bgrSize++] =
                (((int16_t)pointCloud_video_data[i + 1])) / ((Max_Y));
            normalized_vertices[bgrSize++] =
                (((int16_t)pointCloud_video_data[i + 2])) / ((Max_Z));
            if ((int16_t)pointCloud_video_data[i + 2] == 0) {
                normalized_vertices[bgrSize++] = 0.0; //R = 0.0
                normalized_vertices[bgrSize++] = 0.0; //G = 0.0
                normalized_vertices[bgrSize++] = 0.0; //B = 0.0
            } else {
                hsvColorMap((pointCloud_video_data[i + 2]), maxRange, minRange,
                            fRed, fGreen, fBlue);
                normalized_vertices[bgrSize++] = fRed;   //Red
                normalized_vertices[bgrSize++] = fGreen; //Green
                normalized_vertices[bgrSize++] = fBlue;  //Blue
            }
        }

        vertexArraySize =
            pointcloudTableSize * sizeof(float) * 3; //Adding RGB component
        // Create a OpenGL texture identifier
        if (needsInit) {
            needsInit = false;
        }

        std::unique_lock<std::mutex> imshow_lock(m_imshowMutex);
        m_waitKeyBarrier += 1;
        if (m_waitKeyBarrier == numOfThreads) {
            imshow_lock.unlock();
            m_barrierCv.notify_one();
        }
    }
}

void ADIView::hsvColorMap(uint16_t video_data, int max, int min, float &fRed,
                          float &fGreen, float &fBlue) {
    int ClampedValue = video_data;
    ClampedValue = std::min(ClampedValue, max);
    ClampedValue = std::max(ClampedValue, min);
    float hue = 0;
    // The 'hue' coordinate in HSV is a polar coordinate, so it 'wraps'.
    // Purple starts after blue and is close enough to red to be a bit unclear,
    // so we want to go from blue to red.  Purple starts around .6666667,
    // so we want to normalize to [0, .6666667].
    //
    constexpr float range = 2.f / 3.f;
    // Normalize to [0, 1]
    //
    hue = (ClampedValue - min) / static_cast<float>(max - min);
    hue *= range;

    // We want blue to be close and red to be far, so we need to reflect the
    // hue across the middle of the range.
    //
    //hue = range - hue; // Disable HSV format. With this line commented blue is far and red is close.

    fRed = 0.f;
    fGreen = 0.f;
    fBlue = 0.f;
    ImGui::ColorConvertHSVtoRGB(hue, 1.f, 1.f, fRed, fGreen, fBlue);
}

/****************/
//OpenCV  ~deprecated
unsigned int texture;
void ADIView::startCamera() {
    // set up vertex data (and buffer(s)) and configure vertex attributes
    // ------------------------------------------------------------------
    float vertices[] = {
        // positions          // colors       // texture coords
        0.5f,  0.5f,  0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, // top right
        0.5f,  -0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, // bottom right
        -0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, // bottom left
        -0.5f, 0.5f,  0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f  // top left
    };

    // Set up index
    unsigned int indices[] = {
        0, 1, 3, // first triangle
        1, 2, 3  // second triangle
    };

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}
