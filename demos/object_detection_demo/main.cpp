/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <gflags/gflags.h>
#include <algorithm>
#include <functional>
#include <iostream>
#include <fstream>
#include <random>
#include <string>
#include <memory>
#include <vector>
#include <limits>
#include <chrono>

#include <format_reader_ptr.h>
#include <inference_engine.hpp>
#include <ext_list.hpp>

#include <samples/common.hpp>
#include <samples/slog.hpp>
#include <samples/args_helper.hpp>
#include "object_detection_demo.h"
#include "detectionoutput.h"

using namespace InferenceEngine;

bool ParseAndCheckCommandLine(int argc, char *argv[]) {
    // ---------------------------Parsing and validation of input args--------------------------------------
    slog::info << "Parsing input parameters" << slog::endl;

    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
    if (FLAGS_h) {
        showUsage();
        return false;
    }

    if (FLAGS_ni < 1) {
        throw std::logic_error("Parameter -ni should be greater than 0 (default: 1)");
    }

    if (FLAGS_i.empty()) {
        throw std::logic_error("Parameter -i is not set");
    }

    if (FLAGS_m.empty()) {
        throw std::logic_error("Parameter -m is not set");
    }

    return true;
}

/**
* \brief The entry point for the Inference Engine object_detection demo application
* \file object_detection_demo/main.cpp
* \example object_detection_demo/main.cpp
*/
int main(int argc, char *argv[]) {
    try {
        /** This demo covers certain topology and cannot be generalized for any object detection one **/
        slog::info << "InferenceEngine: " << GetInferenceEngineVersion() << "\n";

        // ------------------------------ Parsing and validation of input args ---------------------------------
        if (!ParseAndCheckCommandLine(argc, argv)) {
            return 0;
        }

        /** This vector stores paths to the processed images **/
        std::vector<std::string> images;
        parseImagesArguments(images);
        if (images.empty()) throw std::logic_error("No suitable images were found");
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 1. Load Plugin for inference engine -------------------------------------
        slog::info << "Loading plugin" << slog::endl;
        InferencePlugin plugin = PluginDispatcher({ FLAGS_pp, "../../../lib/intel64" , "" }).getPluginByDevice(FLAGS_d);

        /*If CPU device, load default library with extensions that comes with the product*/
        if (FLAGS_d.find("CPU") != std::string::npos) {
            /**
            * cpu_extensions library is compiled from "extension" folder containing
            * custom MKLDNNPlugin layer implementations. These layers are not supported
            * by mkldnn, but they can be useful for inferencing custom topologies.
            **/
            plugin.AddExtension(std::make_shared<Extensions::Cpu::CpuExtensions>());
        }

        if (!FLAGS_l.empty()) {
            // CPU(MKLDNN) extensions are loaded as a shared library and passed as a pointer to base extension
            IExtensionPtr extension_ptr = make_so_pointer<IExtension>(FLAGS_l);
            plugin.AddExtension(extension_ptr);
            slog::info << "CPU Extension loaded: " << FLAGS_l << slog::endl;
        }

        if (!FLAGS_c.empty()) {
            // clDNN Extensions are loaded from an .xml description and OpenCL kernel files
            plugin.SetConfig({ { PluginConfigParams::KEY_CONFIG_FILE, FLAGS_c } });
            slog::info << "GPU Extension loaded: " << FLAGS_c << slog::endl;
        }

        /** Setting plugin parameter for per layer metrics **/
        if (FLAGS_pc) {
            plugin.SetConfig({ { PluginConfigParams::KEY_PERF_COUNT, PluginConfigParams::YES } });
        }

        /** Printing plugin version **/
        printPluginVersion(plugin, std::cout);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 2. Read IR Generated by ModelOptimizer (.xml and .bin files) ------------
        std::string binFileName = fileNameNoExt(FLAGS_m) + ".bin";
        slog::info << "Loading network files:"
            "\n\t" << FLAGS_m <<
            "\n\t" << binFileName <<
            slog::endl;

        CNNNetReader networkReader;
        /** Read network model **/
        networkReader.ReadNetwork(FLAGS_m);

        /** Extract model name and load weigts **/
        networkReader.ReadWeights(binFileName);
        CNNNetwork network = networkReader.getNetwork();

        Precision p = network.getPrecision();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 3. Configure input & output ---------------------------------------------

        // ------------------------------ Adding DetectionOutput -----------------------------------------------

        /**
         * The only meaningful difference between Faster-RCNN and SSD-like topologies is the interpretation
         * of the output data. Faster-RCNN has 2 output layers which (the same format) are presented inside SSD.
         *
         * But SSD has an additional post-processing DetectionOutput layer that simplifies output filtering.
         * So here we are adding 3 Reshapes and the DetectionOutput to the end of Faster-RCNN so it will return the
         * same result as SSD and we can easily parse it.
         */

        std::string firstLayerName = network.getInputsInfo().begin()->first;

        int inputWidth = network.getInputsInfo().begin()->second->getTensorDesc().getDims()[3];
        int inputHeight = network.getInputsInfo().begin()->second->getTensorDesc().getDims()[2];

        DataPtr bbox_pred_reshapeInPort = ((ICNNNetwork&)network).getData(FLAGS_bbox_name.c_str());
        if (bbox_pred_reshapeInPort == nullptr) {
            throw std::logic_error(std::string("Can't find output layer named ") + FLAGS_bbox_name);
        }

        SizeVector bbox_pred_reshapeOutDims = {
            bbox_pred_reshapeInPort->getTensorDesc().getDims()[0] *
            bbox_pred_reshapeInPort->getTensorDesc().getDims()[1], 1
        };
        DataPtr rois_reshapeInPort = ((ICNNNetwork&)network).getData(FLAGS_proposal_name.c_str());
        if (rois_reshapeInPort == nullptr) {
            throw std::logic_error(std::string("Can't find output layer named ") + FLAGS_proposal_name);
        }

        SizeVector rois_reshapeOutDims = {rois_reshapeInPort->getTensorDesc().getDims()[0] * rois_reshapeInPort->getTensorDesc().getDims()[1], 1};

        DataPtr cls_prob_reshapeInPort = ((ICNNNetwork&)network).getData(FLAGS_prob_name.c_str());
        if (cls_prob_reshapeInPort == nullptr) {
            throw std::logic_error(std::string("Can't find output layer named ") + FLAGS_prob_name);
        }

        SizeVector cls_prob_reshapeOutDims = {cls_prob_reshapeInPort->getTensorDesc().getDims()[0] * cls_prob_reshapeInPort->getTensorDesc().getDims()[1], 1};

        /*
            Detection output
        */

        int normalized = 0;
        int prior_size = normalized ? 4 : 5;
        int num_priors = rois_reshapeOutDims[0] / prior_size;

        // num_classes guessed from the output dims
        if (bbox_pred_reshapeOutDims[0] % (num_priors * 4) != 0) {
            throw std::logic_error("Can't guess number of classes. Something's wrong with output layers dims");
        }
        int num_classes = bbox_pred_reshapeOutDims[0] / (num_priors * 4);
        slog::info << "num_classes guessed: " << num_classes << slog::endl;

        LayerParams detectionOutParams;
        detectionOutParams.name = "detection_out";
        detectionOutParams.type = "DetectionOutput";
        detectionOutParams.precision = p;
        CNNLayerPtr detectionOutLayer = CNNLayerPtr(new CNNLayer(detectionOutParams));
        detectionOutLayer->params["background_label_id"] = "0";
        detectionOutLayer->params["code_type"] = "caffe.PriorBoxParameter.CENTER_SIZE";
        detectionOutLayer->params["eta"] = "1.0";
        detectionOutLayer->params["input_height"] = std::to_string(inputHeight);
        detectionOutLayer->params["input_width"] = std::to_string(inputWidth);
        detectionOutLayer->params["keep_top_k"] = "200";
        detectionOutLayer->params["nms_threshold"] = "0.3";
        detectionOutLayer->params["normalized"] = std::to_string(normalized);
        detectionOutLayer->params["num_classes"] = std::to_string(num_classes);
        detectionOutLayer->params["share_location"] = "0";
        detectionOutLayer->params["top_k"] = "400";
        detectionOutLayer->params["variance_encoded_in_target"] = "1";
        detectionOutLayer->params["visualize"] = "False";

        detectionOutLayer->insData.push_back(bbox_pred_reshapeInPort);
        detectionOutLayer->insData.push_back(cls_prob_reshapeInPort);
        detectionOutLayer->insData.push_back(rois_reshapeInPort);

        SizeVector detectionOutLayerOutDims = {7, 200, 1, 1};
        DataPtr detectionOutLayerOutPort = DataPtr(new Data("detection_out", detectionOutLayerOutDims, p,
                                                            TensorDesc::getLayoutByDims(detectionOutLayerOutDims)));
        detectionOutLayerOutPort->creatorLayer = detectionOutLayer;
        detectionOutLayer->outData.push_back(detectionOutLayerOutPort);

        DetectionOutputPostProcessor detOutPostProcessor(detectionOutLayer.get());

        network.addOutput(FLAGS_bbox_name, 0);
        network.addOutput(FLAGS_prob_name, 0);
        network.addOutput(FLAGS_proposal_name, 0);

        // --------------------------- Prepare input blobs -----------------------------------------------------
        slog::info << "Preparing input blobs" << slog::endl;

        /** Taking information about all topology inputs **/
        InputsDataMap inputsInfo(network.getInputsInfo());

        /** SSD network has one input and one output **/
        if (inputsInfo.size() != 1 && inputsInfo.size() != 2) throw std::logic_error("Demo supports topologies only with 1 or 2 inputs");

        std::string imageInputName, imInfoInputName;

        InputInfo::Ptr inputInfo = inputsInfo.begin()->second;

        SizeVector inputImageDims;
        /** Stores input image **/

        /** Iterating over all input blobs **/
        for (auto & item : inputsInfo) {
            /** Working with first input tensor that stores image **/
            if (item.second->getInputData()->getTensorDesc().getDims().size() == 4) {
                imageInputName = item.first;

                slog::info << "Batch size is " << std::to_string(networkReader.getNetwork().getBatchSize()) << slog::endl;

                /** Creating first input blob **/
                Precision inputPrecision = Precision::U8;
                item.second->setPrecision(inputPrecision);

            } else if (item.second->getInputData()->getTensorDesc().getDims().size() == 2) {
                imInfoInputName = item.first;

                Precision inputPrecision = Precision::FP32;
                item.second->setPrecision(inputPrecision);
                if ((item.second->getTensorDesc().getDims()[1] != 3 && item.second->getTensorDesc().getDims()[1] != 6) ||
                     item.second->getTensorDesc().getDims()[0] != 1) {
                    throw std::logic_error("Invalid input info. Should be 3 or 6 values length");
                }
            }
        }

        // ------------------------------ Prepare output blobs -------------------------------------------------
        slog::info << "Preparing output blobs" << slog::endl;

        OutputsDataMap outputsInfo(network.getOutputsInfo());

        const int maxProposalCount = detectionOutLayerOutDims[1];
        const int objectSize = detectionOutLayerOutDims[0];

        /** Set the precision of output data provided by the user, should be called before load of the network to the plugin **/

        outputsInfo[FLAGS_bbox_name]->setPrecision(Precision::FP32);
        outputsInfo[FLAGS_prob_name]->setPrecision(Precision::FP32);
        outputsInfo[FLAGS_proposal_name]->setPrecision(Precision::FP32);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 4. Loading model to the plugin ------------------------------------------
        slog::info << "Loading model to the plugin" << slog::endl;

        ExecutableNetwork executable_network = plugin.LoadNetwork(network, {});
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 5. Create infer request -------------------------------------------------
        InferRequest infer_request = executable_network.CreateInferRequest();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 6. Prepare input --------------------------------------------------------
        /** Collect images data ptrs **/
        std::vector<std::shared_ptr<unsigned char>> imagesData, originalImagesData;
        std::vector<int> imageWidths, imageHeights;
        for (auto & i : images) {
            FormatReader::ReaderPtr reader(i.c_str());
            if (reader.get() == nullptr) {
                slog::warn << "Image " + i + " cannot be read!" << slog::endl;
                continue;
            }
            /** Store image data **/
            std::shared_ptr<unsigned char> originalData(reader->getData());
            std::shared_ptr<unsigned char> data(reader->getData(inputInfo->getTensorDesc().getDims()[3], inputInfo->getTensorDesc().getDims()[2]));
            if (data.get() != nullptr) {
                originalImagesData.push_back(originalData);
                imagesData.push_back(data);
                imageWidths.push_back(reader->width());
                imageHeights.push_back(reader->height());
            }
        }
        if (imagesData.empty()) throw std::logic_error("Valid input images were not found!");

        size_t batchSize = network.getBatchSize();
        slog::info << "Batch size is " << std::to_string(batchSize) << slog::endl;
        if (batchSize != imagesData.size()) {
            slog::warn << "Number of images " + std::to_string(imagesData.size()) + \
                " doesn't match batch size " + std::to_string(batchSize) << slog::endl;
            slog::warn << std::to_string(std::min(imagesData.size(), batchSize)) + \
                " images will be processed" << slog::endl;
            batchSize = std::min(batchSize, imagesData.size());
        }

        /** Creating input blob **/
        Blob::Ptr imageInput = infer_request.GetBlob(imageInputName);

        /** Filling input tensor with images. First b channel, then g and r channels **/
        size_t num_channels = imageInput->getTensorDesc().getDims()[1];
        size_t image_size = imageInput->getTensorDesc().getDims()[3] * imageInput->getTensorDesc().getDims()[2];

        unsigned char* data = static_cast<unsigned char*>(imageInput->buffer());

        /** Iterate over all input images **/
        for (size_t image_id = 0; image_id < std::min(imagesData.size(), batchSize); ++image_id) {
            /** Iterate over all pixel in image (b,g,r) **/
            for (size_t pid = 0; pid < image_size; pid++) {
                /** Iterate over all channels **/
                for (size_t ch = 0; ch < num_channels; ++ch) {
                    /**          [images stride + channels stride + pixel id ] all in bytes            **/
                    data[image_id * image_size * num_channels + ch * image_size + pid] = imagesData.at(image_id).get()[pid*num_channels + ch];
                }
            }
        }

        if (imInfoInputName != "") {
            Blob::Ptr input2 = infer_request.GetBlob(imInfoInputName);
            auto imInfoDim = inputsInfo.find(imInfoInputName)->second->getTensorDesc().getDims()[1];

            /** Fill input tensor with values **/
            float *p = input2->buffer().as<PrecisionTrait<Precision::FP32>::value_type*>();

            for (size_t image_id = 0; image_id < std::min(imagesData.size(), batchSize); ++image_id) {
                p[image_id * imInfoDim + 0] = static_cast<float>(inputsInfo[imageInputName]->getTensorDesc().getDims()[2]);
                p[image_id * imInfoDim + 1] = static_cast<float>(inputsInfo[imageInputName]->getTensorDesc().getDims()[3]);
                for (int k = 2; k < imInfoDim; k++) {
                    p[image_id * imInfoDim + k] = 1.0f;  // all scale factors are set to 1.0
                }
            }
        }
        // -----------------------------------------------------------------------------------------------------

        // ---------------------------- 7. Do inference --------------------------------------------------------
        slog::info << "Start inference (" << FLAGS_ni << " iterations)" << slog::endl;

        typedef std::chrono::high_resolution_clock Time;
        typedef std::chrono::duration<double, std::ratio<1, 1000>> ms;
        typedef std::chrono::duration<float> fsec;

        double total = 0.0;
        /** Start inference & calc performance **/
        for (int iter = 0; iter < FLAGS_ni; ++iter) {
            auto t0 = Time::now();
            infer_request.Infer();
            auto t1 = Time::now();
            fsec fs = t1 - t0;
            ms d = std::chrono::duration_cast<ms>(fs);
            total += d.count();
        }
        // -----------------------------------------------------------------------------------------------------

        // ---------------------------- 8. Process output ------------------------------------------------------
        slog::info << "Processing output blobs" << slog::endl;

        Blob::Ptr bbox_output_blob = infer_request.GetBlob(FLAGS_bbox_name);
        Blob::Ptr prob_output_blob = infer_request.GetBlob(FLAGS_prob_name);
        Blob::Ptr rois_output_blob = infer_request.GetBlob(FLAGS_proposal_name);

        std::vector<Blob::Ptr> detOutInBlobs = { bbox_output_blob, prob_output_blob, rois_output_blob };

        Blob::Ptr output_blob = std::make_shared<TBlob<float>>(Precision::FP32, Layout::NCHW, detectionOutLayerOutDims);
        output_blob->allocate();
        std::vector<Blob::Ptr> detOutOutBlobs = { output_blob };

        detOutPostProcessor.execute(detOutInBlobs, detOutOutBlobs, nullptr);

        const float* detection = static_cast<PrecisionTrait<Precision::FP32>::value_type*>(output_blob->buffer());

        std::vector<std::vector<int> > boxes(batchSize);
        std::vector<std::vector<int> > classes(batchSize);

        /* Each detection has image_id that denotes processed image */
        for (int curProposal = 0; curProposal < maxProposalCount; curProposal++) {
            float image_id = detection[curProposal * objectSize + 0];
            float label = detection[curProposal * objectSize + 1];
            float confidence = detection[curProposal * objectSize + 2];
            float xmin = detection[curProposal * objectSize + 3] * imageWidths[image_id];
            float ymin = detection[curProposal * objectSize + 4] * imageHeights[image_id];
            float xmax = detection[curProposal * objectSize + 5] * imageWidths[image_id];
            float ymax = detection[curProposal * objectSize + 6] * imageHeights[image_id];

            /* MKLDnn and clDNN have little differente in DetectionOutput layer, so we need this check */
            if (image_id < 0 || confidence == 0) {
                continue;
            }

            std::cout << "[" << curProposal << "," << label << "] element, prob = " << confidence <<
                "    (" << xmin << "," << ymin << ")-(" << xmax << "," << ymax << ")" << " batch id : " << image_id;

            if (confidence > 0.5) {
                /** Drawing only objects with >50% probability **/
                classes[image_id].push_back(static_cast<int>(label));
                boxes[image_id].push_back(static_cast<int>(xmin));
                boxes[image_id].push_back(static_cast<int>(ymin));
                boxes[image_id].push_back(static_cast<int>(xmax - xmin));
                boxes[image_id].push_back(static_cast<int>(ymax - ymin));
                std::cout << " WILL BE PRINTED!";
            }
            std::cout << std::endl;
        }

        for (size_t batch_id = 0; batch_id < batchSize; ++batch_id) {
            addRectangles(originalImagesData[batch_id].get(), imageHeights[batch_id], imageWidths[batch_id], boxes[batch_id], classes[batch_id]);
            const std::string image_path = "out_" + std::to_string(batch_id) + ".bmp";
            if (writeOutputBmp(image_path, originalImagesData[batch_id].get(), imageHeights[batch_id], imageWidths[batch_id])) {
                slog::info << "Image " + image_path + " created!" << slog::endl;
            } else {
                throw std::logic_error(std::string("Can't create a file: ") + image_path);
            }
        }
        // -----------------------------------------------------------------------------------------------------
        std::cout << std::endl << "total inference time: " << total << std::endl;
        std::cout << "Average running time of one iteration: " << total / static_cast<double>(FLAGS_ni) << " ms" << std::endl;
        std::cout << std::endl << "Throughput: " << 1000 * static_cast<double>(FLAGS_ni) * batchSize / total << " FPS" << std::endl;
        std::cout << std::endl;

        /** Show performace results **/
        if (FLAGS_pc) {
            printPerformanceCounts(infer_request, std::cout);
        }
    }
    catch (const std::exception& error) {
        slog::err << error.what() << slog::endl;
        return 1;
    }
    catch (...) {
        slog::err << "Unknown/internal exception happened." << slog::endl;
        return 1;
    }

    slog::info << "Execution successful" << slog::endl;
    return 0;
}
