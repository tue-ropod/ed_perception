#include "perception_plugin_tensorflow.h"

#include <iostream>

#include <ros/package.h>
#include <ros/node_handle.h>

#include <ed/world_model.h>
#include <ed/entity.h>
#include <ed/update_request.h>
#include <ed/error_context.h>
#include <ed/measurement.h>

#include "../plugins/shared_methods.h"

#include <rgbd/Image.h>
#include <rgbd/ros/conversions.h>

#include <tue/filesystem/path.h>

#include <object_recognition_srvs/Recognize.h>


namespace ed
{

namespace perception
{

// ----------------------------------------------------------------------------------------------------

PerceptionPluginTensorflow::PerceptionPluginTensorflow()
{
}

// ----------------------------------------------------------------------------------------------------

PerceptionPluginTensorflow::~PerceptionPluginTensorflow()
{
}

// ----------------------------------------------------------------------------------------------------

void PerceptionPluginTensorflow::initialize(ed::InitData& init)
{
    // Initialize service
    ros::NodeHandle nh_private("~");
    nh_private.setCallbackQueue(&cb_queue_);
    srv_classify_ = nh_private.advertiseService("classify", &PerceptionPluginTensorflow::srvClassify, this);

    ros::NodeHandle nh;
    srv_client_ = nh.serviceClient<object_recognition_srvs::Recognize>("recognize");
}

// ----------------------------------------------------------------------------------------------------

void PerceptionPluginTensorflow::process(const ed::PluginInput& data, ed::UpdateRequest& req)
{
    world_ = &data.world;
    update_req_ = &req;

    cb_queue_.callAvailable();
}

// ----------------------------------------------------------------------------------------------------

bool PerceptionPluginTensorflow::srvClassify(ed_perception::Classify::Request& req, ed_perception::Classify::Response& res)
{

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // Classify

    for(std::vector<std::string>::const_iterator it = req.ids.begin(); it != req.ids.end(); ++it)
    {
        ed::EntityConstPtr e = world_->getEntity(*it);

        // Check if the entity exists
        if (!e)
        {
            res.error_msg += "Entity '" + *it + "' does not exist.\n";
            ROS_ERROR_STREAM(res.error_msg);
            continue;
        }

        // Check if the entity has a measurement associated with it
        if (!e->bestMeasurement())
        {
            res.error_msg += "Entity '" + *it + "' does not have a measurement.\n";
            ROS_ERROR_STREAM(res.error_msg);
            continue;
        }
        MeasurementConstPtr meas_ptr = e->bestMeasurement();

        // Create the classificationrequest and call the service
        object_recognition_srvs::Recognize client_srv;
        cv::Mat image = meas_ptr->image()->getRGBImage();

        // Get the part that is masked
        ed::ImageMask mask = meas_ptr->imageMask();

        cv::Point p_min(image.cols, image.rows);
        cv::Point p_max(0, 0);

        for(ed::ImageMask::const_iterator it2 = mask.begin(image.cols); it2 != mask.end(); ++it2)
        {
            const cv::Point2i& p = *it2;
            p_min.x = std::min(p_min.x, p.x);
            p_min.y = std::min(p_min.y, p.y);
            p_max.x = std::max(p_max.x, p.x);
            p_max.y = std::max(p_max.y, p.y);
        }

        cv::Rect roi = cv::Rect(std::min(p_min.x + 5, image.cols),
                                std::min(p_min.y + 5, image.rows),
                                std::max(p_max.x - p_min.x - 5, 0),
                                std::max(p_max.y - p_min.y - 5, 0));
        cv::Mat cropped_image = image(roi);

        // Convert it to the image request and call the service
        rgbd::convert(cropped_image, client_srv.request.image);
        if (!srv_client_.call(client_srv))
        {
            ROS_ERROR("Service call failed");
            continue;
        }

        // If we have recognitions and the highest probability is above a certain
        // threshold: update the world model
        std::string label;
        double p;
        if (client_srv.response.recognitions.size() > 0)
        {
            p = client_srv.response.recognitions[0].probability;
            if (p > 0.5)  // ToDo: this threshold is now hardcoded. Should we make this configurable
            {
                label = client_srv.response.recognitions[0].label;
                ROS_INFO_STREAM("Entity " + *it + ": " + label);
            }
            else
            {
                ROS_DEBUG_STREAM("Entity " + *it + " not updated, probability too low (" << p << ")");
                continue;
            }

        }
        else
        {
            ROS_WARN_STREAM("No classification for entity " + *it);
            continue;
        }

        // Update the world model
        update_req_->setType(e->id(), label);
//        update_req_->addData(e->id(), data.data()); // What does this do???

        // Add the result to the response
        res.expected_values.push_back(label);
        res.expected_value_probabilities.push_back(p);  // ToDo: does this make any sense?

    }

    return true;
}

// ----------------------------------------------------------------------------------------------------

} // end namespace perception

} // end namespace ed

ED_REGISTER_PLUGIN(ed::perception::PerceptionPluginTensorflow)