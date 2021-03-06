/*
* Copyright (c) 2018 Intel Corporation.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
* LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
* OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
* WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

// std includes
#include <iostream>
#include <stdio.h>
#include <thread>
#include <queue>
#include <map>
#include <atomic>
#include <csignal>
#include <ctime>
#include <mutex>
#include <syslog.h>
#include <string>
#include <fstream>

// OpenCV includes
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>
#include <nlohmann/json.hpp>

// MQTT
#include "mqtt.h"

using namespace std;
using namespace cv;
using namespace dnn;
using json = nlohmann::json;
json jsonobj;


// OpenCV-related variables
Mat frame, displayFrame;
VideoCapture cap;
int delay = 5;
int rate;
// nextImage provides queue for captured video frames
queue<Mat> nextImage;

// flag to control background threads
atomic<bool> keepRunning(true);

// flag to handle UNIX signals
static volatile sig_atomic_t sig_caught = 0;

// mqtt parameters
const string topic = "defects/counter";

// assembly part and defect areas
int min_area;
int max_area;
int total_parts = 0;
int total_defects = 0;
bool prev_seen = false;
bool prev_defect = false;
int frame_defect_count = 0;
int frame_ok_count = 0;

// AssemblyInfo contains information about assembly line defects
struct AssemblyInfo
{
    bool inc_total;
    bool defect;
    int area;
    bool show;
    Rect rect;
};

// currentInfo contains the latest AssemblyInfo as tracked by the application
AssemblyInfo currentInfo = {false};

mutex m, m1, m2;

const char* keys =
    "{ help h      | | Print help message. }"
    "{ minarea min | 20000 | Minimum part area of assembly object. }"
    "{ maxarea max | 30000 | Maximum part area of assembly object. }"
    "{ rate r      | 1 | number of seconds between data updates to MQTT server. }";

// nextImageAvailable returns the next image from the queue in a thread-safe way
Mat nextImageAvailable() {
    Mat rtn;
    m.lock();
    if (!nextImage.empty()) {
        rtn = nextImage.front();
        nextImage.pop();
    }
    m.unlock();

    return rtn;
}

// addImage adds an image to the queue in a thread-safe way
void addImage(Mat img) {
    m.lock();
    if (nextImage.empty()) {
        nextImage.push(img);
    }
    m.unlock();
}

// getCurrentInfo returns the most-recent AssemblyInfo for the application.
AssemblyInfo getCurrentInfo() {
    m2.lock();
    AssemblyInfo info;
    info = currentInfo;
    m2.unlock();

    return info;
}

// updateInfo uppdates the current AssemblyInfo for the application to the latest detected values
void updateInfo(AssemblyInfo info) {
    m2.lock();
    currentInfo.defect = info.defect;
    currentInfo.show = info.show;
    currentInfo.area = info.area;
    currentInfo.rect = info.rect;
    if (info.inc_total) {
        total_parts++;
    }
    if (info.defect) {
        total_defects++;
    }
    m2.unlock();
}

// resetInfo resets the current AssemblyInfo for the application.
void resetInfo() {
    m2.lock();
    currentInfo.defect = false;
    currentInfo.area = 0;
    currentInfo.inc_total = false;
    currentInfo.rect = Rect(0,0,0,0);
    m2.unlock();
}

// publish MQTT message with a JSON payload
void publishMQTTMessage(const string& topic, const AssemblyInfo& info)
{
    ostringstream s;
    s << "{\"Defect\": \"" << info.defect << "\"}";
    string payload = s.str();

    mqtt_publish(topic, payload);

    string msg = "MQTT message published to topic: " + topic;
    syslog(LOG_INFO, "%s", msg.c_str());
    syslog(LOG_INFO, "%s", payload.c_str());
}

// message handler for the MQTT subscription for the any desired control channel topic
int handleMQTTControlMessages(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    string topic = topicName;
    string msg = "MQTT message received: " + topic;
    syslog(LOG_INFO, "%s", msg.c_str());

    return 1;
}

// Function called by worker thread to process the next available video frame.
void frameRunner() {
    while (keepRunning.load()) {
        Mat next = nextImageAvailable();
        if (!next.empty()) {
            Mat img;
            Rect max_rect;
            int max_blob_area = 0;
            int part_area = 0;
            bool defect = false;
            bool frame_defect = false;
            bool inc_total = false;
            Size size(3,3);
            vector<Vec4i> hierarchy;
            vector<vector<Point> > contours;

            cvtColor(frame, img, COLOR_RGB2GRAY);
            // Blur the image to smooth it before easier preprocessing
            GaussianBlur(img, img, size, 0, 0 );

            // Morphology: OPEN -> CLOSE -> OPEN
            // MORPH_OPEN removes the noise and closes the "holes" in the background
            // MORPH_CLOSE remove the noise and closes the "holes" in the foreground
            morphologyEx(img, img, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, size));
            morphologyEx(img, img, MORPH_CLOSE, getStructuringElement(MORPH_ELLIPSE, size));
            morphologyEx(img, img, MORPH_OPEN, getStructuringElement(MORPH_ELLIPSE, size));

            // threshold the image to emphasize assembly part
            threshold(img, img, 200, 255, THRESH_BINARY);
            // find the contours of assembly part
            findContours(img, contours, hierarchy, RETR_EXTERNAL, CHAIN_APPROX_NONE);

            // we will pick detected objects with largest size
            for (size_t i = 0; i < contours.size(); i++)
            {
                Rect rect = boundingRect(contours[i]);
                part_area = rect.width * rect.height;
                // is large enough, and completely within the camera with no overlapping edge.
                if (part_area > max_blob_area && rect.x > 0 && rect.x + rect.width < img.cols && rect.width > 30)
                {
                    max_blob_area = part_area;
                    max_rect = rect;
                }
            }
            part_area = max_blob_area;

            // if no object is detected we dont do anything
            if (part_area != 0) {
                // increment ok or defect counts
                if (part_area > max_area || part_area < min_area)
                {
                    frame_defect = true;
                    frame_defect_count++;
                } else {
                    frame_ok_count++;
                }

                // if the part wasn't seen before it's a new part
                if (!prev_seen) {
                    prev_seen = true;
                    inc_total = true;
                } else {
                    // if the previously seen object has no defect detected in 10 previous consecutive frames
                    if (!frame_defect && frame_ok_count > 10) {
                        frame_defect_count = 0;
                    }
                    // if previously seen object has a defect detected in 10 previous consecutive frames
                    if (frame_defect && frame_defect_count > 10) {
                        if (!prev_defect) {
                            prev_defect = true;
                            defect = true;
                        }
                        frame_ok_count = 0;
                    }
                }
            } else {
                // no part detected -- we are looking at empty belt. reset values.
                prev_seen = false;
                prev_defect = false;
                frame_defect_count = 0;
                frame_ok_count = 0;
            }

            AssemblyInfo info;
            info.defect = defect;
            info.show = prev_defect;
            info.area = part_area;
            info.rect = max_rect;
            info.inc_total = inc_total;

            updateInfo(info);
        }
    }

    cout << "Video processing thread stopped" << endl;
}

// Function called by worker thread to handle MQTT updates. Pauses for rate second(s) between updates.
void messageRunner() {
    while (keepRunning.load()) {
        AssemblyInfo info = getCurrentInfo();
        publishMQTTMessage(topic, info);
        this_thread::sleep_for(chrono::seconds(rate));
    }

    cout << "MQTT sender thread stopped" << endl;
}

// signal handler for the main thread
void handle_sigterm(int signum)
{
    /* we only handle SIGTERM and SIGKILL here */
    if (signum == SIGTERM) {
        cout << "Interrupt signal (" << signum << ") received" << endl;
        sig_caught = 1;
    }
}

int main(int argc, char** argv)
{
    // parse command parameters
    CommandLineParser parser(argc, argv, keys);
    String input;
    std::string conf_file = "../resources/config.json";
    std::ifstream confFile(conf_file);
    confFile>>jsonobj;

    parser.about("Use this script to using OpenVINO.");
    if (argc == 1 || parser.has("help"))
    {
        parser.printMessage();

        return 0;
    }

    min_area = parser.get<int>("minarea");
    max_area = parser.get<int>("maxarea");
    rate = parser.get<int>("rate");

    auto obj = jsonobj["inputs"];
    input = obj[0]["video"];

    if (input.size() == 1 && *(input.c_str()) >= '0' && *(input.c_str()) <= '9')
        cap.open(std::stoi(input));
    else
        cap.open(input);

    if (!cap.isOpened())
    {
        cerr << "ERROR! Unable to open video source\n";
        return -1;
    }

    // Also adjust delay so video playback matches the number of FPS in the file
    double fps = cap.get(CAP_PROP_FPS);
    delay = 1000 / fps;

    // connect MQTT messaging
    int result = mqtt_start(handleMQTTControlMessages);
    if (result == 0) {
        syslog(LOG_INFO, "MQTT started.");
    } else {
        syslog(LOG_INFO, "MQTT NOT started: have you set the ENV varables?");
    }

    mqtt_connect();

    // register SIGTERM signal handler
    signal(SIGTERM, handle_sigterm);

    // start worker threads
    thread t1(frameRunner);
    thread t2(messageRunner);

    string label;
    // read video input data
    for (;;) {
        cap.read(frame);

        if (frame.empty()) {
            keepRunning = false;
            cerr << "ERROR! blank frame grabbed\n";
            break;
        }

        resize(frame, frame, Size(960, 540));
        displayFrame = frame.clone();
        addImage(frame);

        AssemblyInfo info = getCurrentInfo();
        label = format("Measurement: %d Expected range: [%d - %d] Defect: %s",
                        info.area, min_area, max_area, info.defect? "TRUE" : "FALSE");
        putText(displayFrame, label, Point(0, 15), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 255, 0));

        label = format("Total parts: %d Total Defects: %d", total_parts, total_defects);
        putText(displayFrame, label, Point(0, 40), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 255, 0));

        if (info.show) {
            rectangle(displayFrame, info.rect, Scalar(255, 0, 0), 1);
        } else {
            rectangle(displayFrame, info.rect, Scalar(0, 255, 0), 1);
        }

        imshow("Object Size Detector", displayFrame);

        if (waitKey(delay) >= 0 || sig_caught) {
            cout << "Attempting to stop background threads" << endl;
            keepRunning = false;
            break;
        }
    }

    // wait for the threads to finish
    t1.join();
    t2.join();
    cap.release();

    // disconnect MQTT messaging
    mqtt_disconnect();
    mqtt_close();

    return 0;
}
