#include <signal.h>
#include <cstdlib>
#include <string>
#include "project_config.h"

IGNORE_WARNINGS_PUSH

#include <mrpt/gui/CDisplayWindow3D.h>
#include <mrpt/maps/CColouredPointsMap.h>
#include <mrpt/opengl/CGridPlaneXY.h>
#include <mrpt/opengl/stock_objects.h>
#include <mrpt/opengl/CPointCloudColoured.h>

#include <mrpt/gui/CDisplayWindow.h>
#include <mrpt/random.h>
#include <mrpt/bayes/CParticleFilterData.h>
#include <mrpt/obs/CSensoryFrame.h>
#include <mrpt/obs/CObservationImage.h>
#include <mrpt/otherlibs/do_opencv_includes.h>

#include <libfreenect2/libfreenect2.hpp>
#include <libfreenect2/packet_pipeline.h>
#include <libfreenect2/frame_listener_impl.h>
#include <libfreenect2/threading.h>
#include <libfreenect2/registration.h>

#include "ImageRegistration.h"

IGNORE_WARNINGS_POP

//#include "KinectCamera.h"
#include "CImageParticleFilter.h"
#include "misc_helpers.h"
#include "geometry_helpers.h"
#include "ellipse_functions.h"

#include "color_model.h"
#include "faces_detection.h"
#include "model_parameters.h"

using namespace mrpt;
using namespace mrpt::bayes;
using namespace mrpt::gui;
using namespace mrpt::obs;
using namespace mrpt::random;
using namespace mrpt::opengl;
using namespace mrpt::maps;

using namespace std;

libfreenect2::Freenect2Device *dev;
libfreenect2::Freenect2 freenect2;
libfreenect2::SyncMultiFrameListener *listener;
libfreenect2::FrameMap frames_kinect2;
libfreenect2::PacketPipeline *pipeline;

void close_kinect2_handler(void)
{
    listener->waitForNewFrame(frames_kinect2);
    listener->release(frames_kinect2);
    dev->stop();
    dev->close();
}

void dispDepth(const cv::Mat &in, cv::Mat &out, const float maxValue)
{
    cv::Mat tmp = cv::Mat(in.rows, in.cols, CV_8U);
    const uint32_t maxInt = 255;

    #pragma omp parallel for
    for (int r = 0; r < in.rows; ++r) {
        const uint16_t *itI = in.ptr<uint16_t>(r);
        uint8_t *itO = tmp.ptr<uint8_t>(r);

        for (int c = 0; c < in.cols; ++c, ++itI, ++itO) {
            *itO = (uint8_t)std::min((*itI * maxInt / maxValue), 255.0f);
        }
    }

    cv::applyColorMap(tmp, out, cv::COLORMAP_JET);
}

void combine(const cv::Mat &inC, const cv::Mat &inD, cv::Mat &out)
{
    out = cv::Mat(inC.rows, inC.cols, CV_8UC3);

    #pragma omp parallel for
    for (int r = 0; r < inC.rows; ++r) {
        const cv::Vec3b
        *itC = inC.ptr<cv::Vec3b>(r),
         *itD = inD.ptr<cv::Vec3b>(r);
        cv::Vec3b *itO = out.ptr<cv::Vec3b>(r);

        for (int c = 0; c < inC.cols; ++c, ++itC, ++itD, ++itO) {
            itO->val[0] = (itC->val[0] + itD->val[0]) >> 1;
            itO->val[1] = (itC->val[1] + itD->val[1]) >> 1;
            itO->val[2] = (itC->val[2] + itD->val[2]) >> 1;
        }
    }
}

void create_cloud(const std::vector<Eigen::Vector3f> &points, const float scale, CColouredPointsMap &cloud)
{
    cloud.clear();
    const size_t N = points.size();
    //#pragma omp parallel for
    for (size_t i = 0; i < N; i++){
        const Eigen::Vector3f &v = points[i];
        if (isnan(v[2]) || std::abs(v[2] * scale) <= 0.001) {
            continue;
        }
        cloud.insertPoint(-v[2] * scale, v[0] * scale, -v[1] * scale, 0, 1, 0);
    }
}

void create_cloud(const cv::Mat &color, const cv::Mat &depth, const float scale, const ImageRegistration &reg, CColouredPointsMap &cloud)
{
    //const float badPoint = std::numeric_limits<float>::quiet_NaN();
    cloud.clear();
    //#pragma omp parallel for
    for (int r = 0; r < depth.rows; ++r) {
        const uint16_t *itD = depth.ptr<uint16_t>(r);
        const cv::Vec3b *itC = color.ptr<cv::Vec3b>(r);
        const float y = reg.lookupY.at<float>(0, r);
        const float *itX = reg.lookupX.ptr<float>();

        for (size_t c = 0; c < (size_t)depth.cols; ++c, ++itD, ++itC, ++itX) {
            register const float depthValue = *itD;
            // Check for invalid measurements
            if (isnan(depthValue) || std::abs(depthValue) <= 0.001) {
                continue;
            }

            const float z_coord = depthValue * scale;
            const float x_coord = (*itX) * depthValue* scale;
            const float y_coord = y * depthValue* scale;
            //cout << "CLOUD " << x_coord << ' ' << y_coord << ' ' << z_coord << endl;
            cloud.insertPoint(-z_coord, x_coord, -y_coord , itC->val[2] / 255.0, itC->val[1] / 255.0, itC->val[0] / 255.0);
        }
    }
}

struct StateEstimation
{
    float x;
    float y;
    float z;
    float v_x;
    float v_y;
    float v_z;
    int radius_x;
    int radius_y;
    float score_color;
    float score_shape;
    float score_total;
    cv::Point center;
    cv::Rect region;
    cv::Mat color_model;

    StateEstimation():
        x(-1), y(-1), z(-1), v_x(-1), v_y(-1), v_z(-1), radius_x(-1), radius_y(-1), score_color(-1), score_shape(-1), score_total(-1), center(), region(), color_model()
    {
        ;
    };

    StateEstimation(const float x, const float y, const float z, const float v_x, const float v_y, const float v_z,
        const int radius_x, const int radius_y, const float score_color, const float score_shape, const float score_total, const cv::Point &center, const cv::Rect &region,
        const cv::Mat &color_model) :
        x(x), y(y), z(z), v_x(v_x), v_y(v_y), v_z(v_z), radius_x(radius_x), radius_y(radius_y), score_color(score_color), score_shape(score_shape), score_total(score_total),
        center(center), region(region), color_model(color_model)
    {
        ;
    };

    void print(void)
    {
        std::printf("Estate: (%f %f %f) -> (%d %d)\n", x, y, z, radius_x, radius_y);
        std::cout << center << std::endl;
        std::cout << region << std::endl;
    }
};

template <typename DEPTH_TYPE>
void do_tracking(StateEstimation &estate, CParticleFilter &PF, CImageParticleFilter<DEPTH_TYPE> &particles,
    const CSensoryFrame &observation, CParticleFilter::TParticleFilterStats &stats, float &mean_weight)
{
    PF.executeOn(particles, NULL, &observation, &stats);
    mean_weight = particles.get_mean(estate.x, estate.y, estate.z, estate.v_x, estate.v_y, estate.v_z);
    //cout << "ESS_beforeResample " << stats.ESS_beforeResample << " weightsVariance_beforeResample " << stats.weightsVariance_beforeResample << std::endl;
    //cout << "Particle filter ESS: " << particles.ESS() << endl;
}

template <typename DEPTH_TYPE>
void build_visual_model(StateEstimation &state, const ImageRegistration &reg, const cv::Mat &hsv_frame,
    const cv::Mat &depth_frame)
{
    Eigen::Vector2i top_corner, bottom_corner;
    const double center_measured_depth = depth_frame.at<DEPTH_TYPE>(cvRound(state.y), cvRound(state.x));
    const double z = center_measured_depth > 0 ? center_measured_depth : state.z; 
    
    std::tie(top_corner, bottom_corner) = project_model(Eigen::Vector2f(state.x, state.y), z,
        Eigen::Vector2f(0.06, 0.06), reg.cameraMatrixColor, reg.lookupX, reg.lookupY);
    
    state.region = cv::Rect(top_corner[0], top_corner[1], bottom_corner[0] - top_corner[0], bottom_corner[1] - top_corner[1]);
    state.radius_x = (bottom_corner[0] - top_corner[0]) * 0.5;
    state.radius_y = (bottom_corner[1] - top_corner[1]) * 0.5;
    state.center = cv::Point(top_corner[0] + state.radius_x , top_corner[1] + state.radius_y);

    // test whether the estimated roi lies inside of the image frame or not
    const cv::Rect rectangle_image = cv::Rect(0, 0, hsv_frame.cols, hsv_frame.rows);
    const cv::Rect rectangle_image_roi_intersection = rectangle_image & state.region;
    
    if (state.region.area() == rectangle_image_roi_intersection.area()){
        const cv::Mat mask = create_ellipse_mask(state.region, 1);
        cv::Mat hsv_roi = hsv_frame(state.region);
        state.color_model = compute_color_model(hsv_roi, mask);
    }
}

template <typename DEPTH_TYPE>
void score_visual_model(StateEstimation &state, const CImageParticleFilter<DEPTH_TYPE> &particles, const cv::Mat &gradient_vectors)
{
    if (state.color_model.empty()){
        state.score_total = -1;
        state.score_color = -1;
        state.score_shape = -1;
        return;
    }

    state.score_color = 1 - cv::compareHist(state.color_model, particles.color_model, CV_COMP_BHATTACHARYYA);
    state.score_shape = ellipse_contour_test(state.center, state.radius_x, state.radius_y, ELLIPSE_FITTING_ANGLE_STEP, gradient_vectors, cv::Mat(), nullptr);
    state.score_total = state.score_color * state.score_shape;
}


int particle_filter()
{
    string face_cascade_name = "../cascades/lbpcascade_frontalface.xml";
    string eyes_cascade_name = "../cascades/haarcascade_eye_tree_eyeglasses.xml";
    cv::CascadeClassifier face_cascade;
    cv::CascadeClassifier eyes_cascade;

    if (!face_cascade.load(face_cascade_name)) {
        printf("--(!)Error loading\n");
        return -1;
    }

    if (!eyes_cascade.load(eyes_cascade_name)) {
        printf("--(!)Error loading\n");
        return -1;
    }


    if (freenect2.enumerateDevices() == 0) {
        std::cout << "no device connected!" << std::endl;
        return -1;
    }

    std::string serial = freenect2.getDefaultDeviceSerialNumber();

    char *calib_dir = getenv("HOME");
    std::string calib_path = std::string(calib_dir) + "/kinect2_calib/";

    pipeline = new libfreenect2::OpenCLPacketPipeline();

    const float minDepth = 0;
    const float maxDepth = 12.0;
    const bool bilateral_filter = true;
    const bool edge_aware_filter = true;

    libfreenect2::DepthPacketProcessor::Config config;
    config.EnableBilateralFilter = bilateral_filter;
    config.EnableEdgeAwareFilter = edge_aware_filter;
    config.MinDepth = minDepth;
    config.MaxDepth = maxDepth;

    pipeline->getDepthPacketProcessor()->setConfiguration(config);
    dev = freenect2.openDevice(serial, pipeline);

    if (dev == nullptr) {
        std::cout << "no device connected or failure opening the default one!" << std::endl;
        exit(-1);
    }

    listener = new libfreenect2::SyncMultiFrameListener(libfreenect2::Frame::Color | libfreenect2::Frame::Ir | libfreenect2::Frame::Depth);
    dev->setColorFrameListener(listener);
    dev->setIrAndDepthFrameListener(listener);
    dev->start();
    ImageRegistration reg;
    reg.init(calib_path, serial);


    /*
    struct sigaction sigIntHandler;

    sigIntHandler.sa_handler = [](int) {
        std::cout << "SIGINT" << std::endl;
        close_kinect2_handler();
        std::cout << "SIGINT2" << std::endl;
        exit(0);
    };
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;
    sigaction(SIGINT, &sigIntHandler, NULL);
    sigaction(SIGQUIT, &sigIntHandler, NULL);
    sigaction(SIGSEGV, &sigIntHandler, NULL);
    */
    //atexit(close_kinect2_handler);
    //at_quick_exit(close_kinect2_handler);

    cv::Mat color_frame;
    cv::Mat color_display_frame;
    cv::Mat model_frame;
    cv::Mat depth_frame;

    randomGenerator.randomize();

    CDisplayWindow image("image");

    CDisplayWindow model_image_window("model-image");
    CDisplayWindow model_candidate_window("model_candidate_window");
    CDisplayWindow model_histogram_window("model_histogram_window");
    CDisplayWindow model_candidate_histogram_window("model_candidate_histogram_window");
    CDisplayWindow registered_color_window("registered_depth_color");
    CDisplayWindow gradient_depth_window("gradient_depth_window");
    CDisplayWindow gradient_color_window("gradient_color_window");

    CDisplayWindow model_histogram_window2("2model_histogram_window");
    CDisplayWindow model_candidate_histogram_window2("2model_candidate_histogram_window");

    // -------------------3D view stuff------------------

//#define _3D
#ifdef _3D
    CDisplayWindow3D win3D("Kinect 3D view", 640, 480);

    win3D.setCameraAzimuthDeg(0);
    win3D.setCameraElevationDeg(0);
    win3D.setCameraZoom(1);
    win3D.setFOV(65);
    win3D.setCameraPointingToPoint(0, 0, 0);

    mrpt::opengl::CPointCloudColouredPtr scene_points = mrpt::opengl::CPointCloudColoured::Create();
    mrpt::opengl::CPointCloudColouredPtr particle_points = mrpt::opengl::CPointCloudColoured::Create();
    mrpt::opengl::CPointCloudColouredPtr model_center_points = mrpt::opengl::CPointCloudColoured::Create();
    scene_points->setPointSize(0.5);
    particle_points->setPointSize(2);
    model_center_points->setPointSize(2);

    {
        mrpt::opengl::COpenGLScenePtr &scene = win3D.get3DSceneAndLock();
        scene->insert(scene_points);
        scene->insert(particle_points);
        scene->insert(model_center_points);
        scene->insert(mrpt::opengl::CGridPlaneXY::Create());
        //scene->insert(mrpt::opengl::stock_objects::CornerXYZSimple());
        win3D.unlockAccess3DScene();
        win3D.repaint();
    }

    CColouredPointsMap scene_points_map;
    CColouredPointsMap particle_points_map;
    CColouredPointsMap model_center_points_map;
    scene_points_map.colorScheme.scheme = CColouredPointsMap::cmFromIntensityImage;
#endif

    // Create PF
    // ----------------------
    CParticleFilter::TParticleFilterOptions PF_options;
    PF_options.adaptiveSampleSize = false;
    PF_options.PF_algorithm = CParticleFilter::pfStandardProposal;
    //PF_options.resamplingMethod = CParticleFilter::prSystematic;
    PF_options.resamplingMethod = CParticleFilter::prMultinomial;

    CParticleFilter PF;
    PF.m_options = PF_options;

    using DEPTH_TYPE = uint16_t;
    CImageParticleFilter<DEPTH_TYPE> particles;

    bool init_model = true;
    /*
    float x_radius_global;
    float y_radius_global;
    float x_global;
    float y_global;
    */
    while (!mrpt::system::os::kbhit()) {
        listener->waitForNewFrame(frames_kinect2);
        libfreenect2::Frame *rgb = frames_kinect2[libfreenect2::Frame::Color];
        libfreenect2::Frame *depth = frames_kinect2[libfreenect2::Frame::Depth];
        //libfreenect2::Frame *ir = frames_kinect2[libfreenect2::Frame::Ir];


        cv::Mat color_mat = cv::Mat(rgb->height, rgb->width, CV_8UC3, rgb->data);
        cv::Mat depth_mat = cv::Mat(depth->height, depth->width, CV_32FC1, depth->data);
        //cv::Mat ir_mat = cv::Mat(depth->height, depth->width, CV_32FC1, ir->data);

        cv::Mat registered_depth;
        reg.register_images(color_mat, depth_mat, registered_depth);

        cv::flip(color_mat, color_mat, 1);
        cv::Mat depthDisp;
        dispDepth(registered_depth, depthDisp, 12000.0f);
        cv::Mat combined;
        combine(color_mat, depthDisp, combined);
        cv::line(combined, cv::Point(color_display_frame.cols * 0.5, 0), cv::Point(color_display_frame.cols * 0.5, color_display_frame.rows - 1), cv::Scalar(0, 0, 255));
        cv::line(combined, cv::Point(0, color_display_frame.rows * 0.5), cv::Point(color_display_frame.cols - 1, color_display_frame.rows * 0.5), cv::Scalar(0, 255, 0));

        CImage registered_depth_image;
        registered_depth_image.loadFromIplImage(new IplImage(combined));
        registered_color_window.showImage(registered_depth_image);

        color_frame = color_mat;
        depth_frame = registered_depth;
        color_display_frame = color_mat.clone();
        cv::Mat hsv_frame;
        cv::cvtColor(color_frame, hsv_frame, cv::COLOR_BGR2HSV);

        cv::Mat gradient_vectors, gradient_magnitude, gradient_magnitude_scaled;
        std::tie(gradient_vectors, gradient_magnitude, gradient_magnitude_scaled) = sobel_operator(color_frame);
        
        //color_frame = registered_color_mat;

        // Process with PF:

        //TODO: USE CObservationStereoImages?
        CObservationImagePtr obsImage_color = CObservationImage::Create();
        CObservationImagePtr obsImage_hsv = CObservationImage::Create();
        CObservationImagePtr obsImage_depth = CObservationImage::Create();
        CObservationImagePtr obsImage_gradient_vectors = CObservationImage::Create();
        CObservationImagePtr obsImage_gradient_magnitude = CObservationImage::Create();

        obsImage_color->image.loadFromIplImage(new IplImage(color_frame));
        obsImage_color->sensorLabel = "color";

        obsImage_hsv->image.loadFromIplImage(new IplImage(hsv_frame));
        obsImage_hsv->sensorLabel = "hsv";

        obsImage_depth->image.loadFromIplImage(new IplImage(depth_frame));
        obsImage_depth->sensorLabel = "depth";

        obsImage_gradient_vectors->image.loadFromIplImage(new IplImage(gradient_vectors));
        obsImage_gradient_vectors->sensorLabel = "gradient_vectors";

        obsImage_gradient_magnitude->image.loadFromIplImage(new IplImage(gradient_magnitude));
        obsImage_gradient_magnitude->sensorLabel = "gradient_magnitude";
        
        // memory freed by SF.
        CSensoryFrame observation;
        observation.insert(obsImage_color);
        observation.insert(obsImage_hsv);
        observation.insert(obsImage_depth);
        observation.insert(obsImage_gradient_vectors);
        observation.insert(obsImage_gradient_magnitude);

        /*
        double min, max;
        cv::minMaxLoc(depth_frame, &min, &max);
        cv::Mat depth_frame_normalized = (depth_frame * 255)/ max;
        cv::Mat gradient_depth = sobel_operator(depth_frame_normalized);
        cv::Mat gradient_depth_8UC1 = cv::Mat(depth_frame.size(), CV_8UC1);

        CImage gradient_depth_image;
        gradient_depth_image.loadFromIplImage(new IplImage(gradient_depth));
        gradient_depth_window.showImage(gradient_depth_image);
        */

        if (init_model) {
            std::vector<cv::Vec3f> circles = viola_faces::detect_circles(color_frame);

            /*
            std::vector<viola_faces::face> caras = viola_faces::detect_faces(color_frame);
            for (auto &cara : caras){
                circles.push_back(cv::Vec3f(cara.first.x + cara.first.width / 2, cara.first.y + cara.first.height / 2, cara.first.width / 2));
            }
            */

            if (circles.size()) {
                int circle_max = 0;
                double radius_max = circles[0][2];
                for (size_t i = 0; i < circles.size(); i++) {
                    cv::Point center(cvRound(circles[i][0]), cvRound(circles[i][1]));
                    int radius = cvRound(circles[i][2]);
                    cv::circle(color_display_frame, center, 3, cv::Scalar(0, 255, 0), -1, 8, 0);
                    cv::circle(color_display_frame, center, radius, cv::Scalar(0, 0, 255), 3, 8, 0);
                    if (radius_max < radius) {
                        radius_max = radius;
                        circle_max = i;
                    }
                }

                cv::Point center(cvRound(circles[circle_max][0]), cvRound(circles[circle_max][1]));
                //int radius_2d = cvRound(circles[circle_max][2]);

                Eigen::Vector2i top_corner, bottom_corner;

                std::tie(top_corner, bottom_corner) = project_model(
                    Eigen::Vector2f(center.x, center.y),
                    depth_frame.at<DEPTH_TYPE>(cvRound(center.y), cvRound(center.x)),
                    Eigen::Vector2f(0.06, 0.06),
                    reg.cameraMatrixColor, reg.lookupX, reg.lookupY);

                float x_radius = (bottom_corner - top_corner)[0] * 0.5;
                float y_radius = (bottom_corner - top_corner)[1] * 0.5;

                //const cv::Rect model_roi(center.x - radius, center.y - radius, 2 * radius, 2 * radius);
                const cv::Rect model_roi(top_corner[0], top_corner[1],
                    (bottom_corner[0] - top_corner[0]),
                    (bottom_corner[1] - top_corner[1])
                );

                const cv::Mat mask = create_ellipse_mask(model_roi, 1);
                cv::Mat hsv_roi = hsv_frame(model_roi);
                const cv::Mat model = compute_color_model2(hsv_roi, mask);
                
                { 
                    const cv::Mat model2 = compute_color_model(hsv_roi, mask);
                    //CImage model_histogram_image2;
                    //model_histogram_image2.loadFromIplImage(new IplImage(histogram_to_image(model2, 10)));
                    //model_histogram_window2.showImage(model_histogram_image2);
                    cout << "SON IGUALES? " << model2.size() << ' ' << model.size() << std::endl;
                    cout << "SON IGUALES? " << type2str(model2.type()) << ' ' << type2str(model.type()) << std::endl;
                    cout << "SON IGUALES? " << cv::norm(model2, model) << std::endl;
                    assert(cv::norm(model2, model) == 0);
                }


                particles.update_color_model(model);

                std::cout << "MEAN detected circle: " << center.x << ' ' << center.y << ' ' << depth_frame.at<DEPTH_TYPE>(cvRound(center.y), cvRound(center.x)) << std::endl;
                particles.initializeParticles(NUM_PARTICLES,
                    make_pair(center.x, x_radius), make_pair(center.y, y_radius), make_pair(float(depth_frame.at<DEPTH_TYPE>(cvRound(center.y), cvRound(center.x))), 100.f),
                    //make_pair(center.x, x_radius), make_pair(center.y, y_radius), make_pair(0, 250.f),
                    //make_pair(center.x, 0), make_pair(center.y, 0), make_pair(0, 25),
                    //make_pair(0, 500), make_pair(0, 500), make_pair(0, 500),
                    make_pair(0, 0), make_pair(0, 0), make_pair(0, 0),
                    make_pair(0.12, 0.12),
                    reg
                );

                init_model = false;

                model_frame = cv::Mat(color_frame(model_roi).size(), color_frame.type());
                const cv::Mat ones = cv::Mat::ones(color_frame(model_roi).size(), color_frame(model_roi).type());
                bitwise_and(color_frame(model_roi), ones, model_frame, mask);

                CImage model_frame;
                model_frame.loadFromIplImage(new IplImage(color_frame(model_roi)));
                model_image_window.showImage(model_frame);
                CImage model_histogram_image;
                model_histogram_image.loadFromIplImage(new IplImage(histogram_to_image(particles.color_model, 10)));
                model_histogram_window.showImage(model_histogram_image);
                //mrpt::system::os::getch();

            }

        } else {
            StateEstimation estimated_state;
            static CParticleFilter::TParticleFilterStats stats;
            
            float mean_weight;
            do_tracking(estimated_state, PF, particles, observation, stats, mean_weight);
            build_visual_model<DEPTH_TYPE>(estimated_state, reg, hsv_frame, depth_frame);

            CImage model_candidate;
            model_candidate.loadFromIplImage(new IplImage(color_frame(estimated_state.region)));
            //model_candidate.loadFromIplImage(new IplImage(w_mask_img));
            model_candidate_window.showImage(model_candidate);

            CImage model_candidate_histogram_image;
            model_candidate_histogram_image.loadFromIplImage(new IplImage(histogram_to_image(estimated_state.color_model, 10)));
            model_candidate_histogram_window.showImage(model_candidate_histogram_image);

            score_visual_model(estimated_state, particles, gradient_vectors);

            //std::cout << "MEAN " << mean_weight << " " << estimated_state.x << ' ' << estimated_state.y << ' ' << estimated_state.z << ' '
                    //<< estimated_state.v_x << ' ' << estimated_state.v_y << ' ' << estimated_state.v_z << std::endl;
            
            cv::circle(color_display_frame, cv::Point(estimated_state.x, estimated_state.y), 20, cv::Scalar(255, 255, 0), 5, 1, 0);
            cv::circle(color_display_frame, cv::Point(estimated_state.x, estimated_state.y), 20, cv::Scalar(255, 0, 0), 5, 1, 0);
            cv::line(color_display_frame, cv::Point(estimated_state.x, estimated_state.y), cv::Point(estimated_state.x + estimated_state.v_x, estimated_state.y + estimated_state.v_y), cv::Scalar(0, 255, 0), 5, 1, 0);
            //std::cout << "SCORE: " << estimated_state.score_total <<  " " << estimated_state.score_shape << '*' << estimated_state.score_color << std::endl;
            
            if (estimated_state.score_total > LIKEHOOD_FOUND){
                cv::circle(color_display_frame, estimated_state.center, 3, cv::Scalar(0, 255, 0), -1, 8, 0);
                cv::circle(color_display_frame, estimated_state.center, (estimated_state.radius_x + estimated_state.radius_y) * 0.5, cv::Scalar(0, 0, 255), 3, 8, 0);
                //cv::circle(gradient_magnitude, center, (radius_x + radius_y) * 0.5 * 1.2, cv::Scalar(128, 128, 128), 3, 8, 0);
                //cv::circle(gradient_magnitude, center, 3, cv::Scalar(0, 255, 0), -1, 8, 0);
            }

            if (estimated_state.score_total > LIKEHOOD_UPDATE){
                CImage model_frame;
                particles.update_color_model(estimated_state.color_model);
                model_frame.loadFromIplImage(new IplImage(color_frame(estimated_state.region)));
                model_image_window.showImage(model_frame);
            }

            std::ostringstream oss;
            oss << "SCORE:" << estimated_state.score_total << " ";
            oss << "BHAT: " << estimated_state.score_color << " ";
            oss << "SHAPE: " << estimated_state.score_shape << " ";
             const cv::Point frame_center(gradient_magnitude.cols / 2 , gradient_magnitude.rows/2);
            
            float fitting = ellipse_contour_test(frame_center, estimated_state.radius_x, estimated_state.radius_y, ELLIPSE_FITTING_ANGLE_STEP, gradient_vectors, gradient_magnitude, &color_display_frame);
            
            float fitting_01 = ellipse_contour_test(frame_center, estimated_state.radius_x, estimated_state.radius_y, ELLIPSE_FITTING_ANGLE_STEP, gradient_vectors, cv::Mat(), &gradient_magnitude_scaled);
            
            ellipse_contour_test(estimated_state.center, estimated_state.radius_x, estimated_state.radius_y, ELLIPSE_FITTING_ANGLE_STEP, gradient_vectors, cv::Mat(), &gradient_magnitude_scaled);
            
            oss << "SHAPE CENTER: " << fitting_01 << " (" << fitting << ")";
            {
                int fontFace =  cv::FONT_HERSHEY_PLAIN;
                double fontScale = 2;
                int thickness = 2;

                int baseline = 0;
                cv::Size textSize = cv::getTextSize(oss.str(), fontFace, fontScale, thickness, &baseline);
                cv::Point textOrg(0, textSize.height + 10);
                //cv::Point textOrg(textSize.width, textSize.height);
                putText(color_display_frame, oss.str(), textOrg, fontFace, fontScale, cv::Scalar(255, 255, 0), thickness, 8);
            }
            /*
            {

                Eigen::Vector2i top_corner, bottom_corner;
                std::tie(top_corner, bottom_corner) = project_model(Eigen::Vector2f(avrg_x, avrg_y),
                    depth_frame.at<DEPTH_TYPE>(cvRound(avrg_y), cvRound(avrg_x)),
                    Eigen::Vector2f(0.06*PERCENTAGE, 0.06*PERCENTAGE), reg.cameraMatrixColor, reg.lookupX, reg.lookupY);

                const cv::Rect model_roi(top_corner[0], top_corner[1], bottom_corner[0] - top_corner[0], bottom_corner[1] - top_corner[1]);
                const int radius_x = (bottom_corner[0] - top_corner[0]) * 0.5;
                const int radius_y = (bottom_corner[1] - top_corner[1]) * 0.5;
                const cv::Point center(top_corner[0] + radius_x , top_corner[1] + radius_y);

                // test whether the estimated roi lies inside of the image frame or not
                const cv::Rect rectangle_image = cv::Rect(cv::Point(0, 0), color_frame.size());
                const cv::Rect rectangle_image_roi_intersection = rectangle_image & model_roi;
                if (model_roi.area() == rectangle_image_roi_intersection.area()){
                    const cv::Mat mask = create_ellipse_mask(model_roi, 1);
                    cv::Mat hsv_roi = hsv_frame(model_roi);

                    const cv::Mat model = compute_color_model(hsv_roi, mask);
                    const double distance_hist = cv::compareHist(model, particles.color_model, CV_COMP_BHATTACHARYYA);
                    const double score = 1 - distance_hist;
                    //const double score = (1.0f / (SQRT_2PI * SIGMA_COLOR)) * exp(-0.5f * distance_hist * distance_hist / (SIGMA_COLOR * SIGMA_COLOR));
                    std::ostringstream oss;
                    oss << "BHATTACHARYYA: " << score << " ";

                    //std::cout << "BHATTACHARYYA: " << score << std::endl;

                    if (radius_x != 0 && radius_y != 0){
                        //float fitting_magnitude = ellipse_contour_test(center, radius_x * 1.0/PERCENTAGE, radius_y * 1.0/PERCENTAGE, ELLIPSE_FITTING_ANGLE_STEP, gradient_vectors, gradient_magnitude, &color_display_frame);
                        //float fitting = ellipse_contour_test(center, radius_x * 1.0/PERCENTAGE, radius_y * 1.0/PERCENTAGE, ELLIPSE_FITTING_ANGLE_STEP, gradient_vectors, cv::Mat(), &color_display_frame);
                        //oss << "FITTING: " << fitting << "(" << fitting_magnitude << ") ";
                        //std::cout << "FITTING  SCORE " << fitting << std::endl;
                        //std::cout << "RADIUS " << radius_x << ' ' << radius_y << std::endl;
                    }

                    //cv::Mat w_mask = create_ellipse_weight_mask(mask);
                    cv::Mat w_mask = create_ellipse_mask(cv::Rect(0, 0, 100, 200), 1);
                    cv::Scalar sum_w = cv::sum(w_mask);
                    cv::Mat w_mask_img;
                    cv::convertScaleAbs(w_mask, w_mask_img, 255.0);
                    //w_mask_img = w_mask.clone() * 255;

                    CImage model_candidate;
                    //model_candidate.loadFromIplImage(new IplImage(color_frame(model_roi)));
                    model_candidate.loadFromIplImage(new IplImage(w_mask_img));
                    model_candidate_window.showImage(model_candidate);

                    CImage model_candidate_histogram_image;
                    model_candidate_histogram_image.loadFromIplImage(new IplImage(histogram_to_image(model, 10)));
                    model_candidate_histogram_window.showImage(model_candidate_histogram_image);


                    if (score > LIKEHOOD_FOUND){
                        cv::circle(color_display_frame, center, 3, cv::Scalar(0, 255, 0), -1, 8, 0);
                        cv::circle(color_display_frame, center, (radius_x + radius_y) * 0.5, cv::Scalar(0, 0, 255), 3, 8, 0);

                        //cv::circle(gradient_magnitude, center, (radius_x + radius_y) * 0.5 * 1.2, cv::Scalar(128, 128, 128), 3, 8, 0);
                        //cv::circle(gradient_magnitude, center, 3, cv::Scalar(0, 255, 0), -1, 8, 0);
                    }

                    if (score > LIKEHOOD_UPDATE){
                        std::cout << "MEAN UPDATING MODEL" << std::endl;
                        CImage model_frame;
                        model_frame.loadFromIplImage(new IplImage(color_frame(model_roi)));
                        model_image_window.showImage(model_frame);
                        particles.update_color_model(model);
                    }

                    const cv::Point center(gradient_magnitude.cols / 2 , gradient_magnitude.rows/2);
                    
                    float fitting = ellipse_contour_test(center, x_radius_global, y_radius_global, ELLIPSE_FITTING_ANGLE_STEP, gradient_vectors, gradient_magnitude, &color_display_frame);
                    
                    float fitting_01 = ellipse_contour_test(center, x_radius_global, y_radius_global, ELLIPSE_FITTING_ANGLE_STEP, gradient_vectors, cv::Mat(), &gradient_magnitude_scaled);
                    oss << "FITTING CENTER: " << fitting_01 << " (" << fitting << ")";
                    {
                        int fontFace =  cv::FONT_HERSHEY_PLAIN;
                        double fontScale = 2;
                        int thickness = 2;

                        int baseline = 0;
                        cv::Size textSize = cv::getTextSize(oss.str(), fontFace, fontScale, thickness, &baseline);
                        cv::Point textOrg(0, textSize.height + 10);
                        //cv::Point textOrg(textSize.width, textSize.height);
                        putText(color_display_frame, oss.str(), textOrg, fontFace, fontScale, cv::Scalar(255, 255, 0), thickness, 8);
                    }
                    
                }


                
                //particles.initializeParticles(NUM_PARTICLES,
                    //make_pair(avrg_x, x_radius), make_pair(avrg_y, y_radius), make_pair(float(depth_frame.at<uint16_t>(cvRound(avrg_y), cvRound(avrg_x))), 1000.f),
                    //make_pair(0, 0), make_pair(0, 0), make_pair(0, 0),
                    //make_pair(0.12, 0.12), reg);

            }
            */
        }

        particles.last_time = cv::getTickCount();

        size_t N = particles.m_particles.size();


        for (size_t i = 0; i < N; i++) {
            cv::circle(color_display_frame, cv::Point(particles.m_particles[i].d->x,
                                              particles.m_particles[i].d->y), 1, cv::Scalar(0, 0, 255), 1, 1, 0);
        }

        cv::line(color_display_frame, cv::Point(color_display_frame.cols * 0.5, 0), cv::Point(color_display_frame.cols * 0.5, color_display_frame.rows - 1), cv::Scalar(0, 0, 255));
        cv::line(color_display_frame, cv::Point(0, color_display_frame.rows * 0.5), cv::Point(color_display_frame.cols - 1, color_display_frame.rows * 0.5), cv::Scalar(0, 255, 0));

        CImage gradient_magnitude_image;
        gradient_magnitude_image.loadFromIplImage(new IplImage(gradient_magnitude_scaled));
        gradient_color_window.showImage(gradient_magnitude_image);

        CImage frame_particles;
        frame_particles.loadFromIplImage(new IplImage(color_display_frame));
        image.showImage(frame_particles);

#ifdef _3D

        //--- 3D view stuff
        create_cloud(color_frame, depth_frame, 1.0/1000.0f, reg, scene_points_map);

        //std::vector<Eigen::Vector2f> particle_vectors(N);
        std::vector<Eigen::Vector3f> particle_vectors3d(N);
        std::vector<Eigen::Vector3f> particle_vectors3d_2(N);

        for (size_t i = 0; i < N; i++) {
            //particle_vectors[i] = Eigen::Vector2f(particles.m_particles[i].d->x, particles.m_particles[i].d->y);
            particle_vectors3d_2[i] = Eigen::Vector3f(particles.m_particles[i].d->x, particles.m_particles[i].d->y, particles.m_particles[i].d->z);
            //particle_vectors3d[i] = point_3D_reprojection(particle_vectors[i], particles.m_particles[i].d->z, reg.lookupX, reg.lookupY);
        }

        //std::vector<Eigen::Vector3f> points_3d = points_3D_reprojection<DEPTH_DATA_TYPE>(particle_vectors, depth_frame, reg.lookupX, reg.lookupY);
        std::vector<Eigen::Vector3f> particle_points_3d = pixel_depth_to_3D_coordiantes(particle_vectors3d_2, reg.cameraMatrixColor);
        //create_cloud(points_3d, particle_points_map);
        //create_cloud(particle_vectors3d, particle_points_map);
        //points_3d[0] = pixel_depth_to_3D_coordiantes(Eigen::Vector3f(x_global, y_global, depth_frame.at<DEPTH_TYPE>(y_global, x_global)), reg.cameraMatrixColor);
        create_cloud(particle_points_3d, 1.0/1000.0f, particle_points_map);

        win3D.get3DSceneAndLock();
        scene_points->loadFromPointsMap(&scene_points_map);
        particle_points->loadFromPointsMap(&particle_points_map);
        //model_center_points->loadFromPointsMap(&model_center_points_map);
        win3D.unlockAccess3DScene();
        win3D.repaint();
#endif

        //mrpt::system::sleep(1);
        listener->release(frames_kinect2);
    }

    listener->waitForNewFrame(frames_kinect2);
    listener->release(frames_kinect2);
    dev->stop();
    dev->close();
    return 0;
}


int main(int argc, char *argv[])
{
    if (argc > 1) {
        NUM_PARTICLES = atof(argv[1]);
    } else {
        NUM_PARTICLES = 1000;
    }

    if (argc > 2) {
        TRANSITION_MODEL_STD_XY = atof(argv[2]);
    } else {
        TRANSITION_MODEL_STD_XY = 10;
    }

    if (argc > 3) {
        TRANSITION_MODEL_STD_VXY  = atof(argv[3]);
    } else {
        TRANSITION_MODEL_STD_VXY  = 10;
    }

    std::cout << "NUM_PARTICLES: " << NUM_PARTICLES << " TRANSITION_MODEL_STD_XY: " << TRANSITION_MODEL_STD_XY << " TRANSITION_MODEL_STD_VXY: " << TRANSITION_MODEL_STD_VXY << std::endl;


    particle_filter();

    return 0;

    /*
    try {
        particle_filter();
        return 0;
    } catch (std::exception &e) {
        std::cout << "MRPT exception caught: " << e.what() << std::endl;
        return -1;
    } catch (...) {
        printf("Untyped exception!!");
        return -1;
    }
    */
}

