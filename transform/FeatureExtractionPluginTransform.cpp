/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Sonic Visualiser
    An audio file viewer and annotation editor.
    Centre for Digital Music, Queen Mary, University of London.
    This file copyright 2006 Chris Cannam.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "FeatureExtractionPluginTransform.h"

#include "plugin/FeatureExtractionPluginFactory.h"
#include "plugin/PluginXml.h"
#include "vamp-sdk/Plugin.h"

#include "data/model/Model.h"
#include "base/Window.h"
#include "data/model/SparseOneDimensionalModel.h"
#include "data/model/SparseTimeValueModel.h"
#include "data/model/EditableDenseThreeDimensionalModel.h"
#include "data/model/DenseTimeValueModel.h"
#include "data/model/NoteModel.h"
#include "data/model/FFTModel.h"

#include <fftw3.h>

#include <iostream>

FeatureExtractionPluginTransform::FeatureExtractionPluginTransform(Model *inputModel,
								   QString pluginId,
                                                                   int channel,
                                                                   QString configurationXml,
								   QString outputName) :
    Transform(inputModel),
    m_plugin(0),
    m_channel(channel),
    m_stepSize(0),
    m_blockSize(0),
    m_descriptor(0),
    m_outputFeatureNo(0)
{
//    std::cerr << "FeatureExtractionPluginTransform::FeatureExtractionPluginTransform: plugin " << pluginId.toStdString() << ", outputName " << outputName.toStdString() << std::endl;

    FeatureExtractionPluginFactory *factory =
	FeatureExtractionPluginFactory::instanceFor(pluginId);

    if (!factory) {
	std::cerr << "FeatureExtractionPluginTransform: No factory available for plugin id \""
		  << pluginId.toStdString() << "\"" << std::endl;
	return;
    }

    m_plugin = factory->instantiatePlugin(pluginId, m_input->getSampleRate());

    if (!m_plugin) {
	std::cerr << "FeatureExtractionPluginTransform: Failed to instantiate plugin \""
		  << pluginId.toStdString() << "\"" << std::endl;
	return;
    }

    if (configurationXml != "") {
        PluginXml(m_plugin).setParametersFromXml(configurationXml);
    }

    m_blockSize = m_plugin->getPreferredBlockSize();
    m_stepSize = m_plugin->getPreferredStepSize();

    if (m_blockSize == 0) m_blockSize = 1024; //!!! todo: ask user
    if (m_stepSize == 0) m_stepSize = m_blockSize; //!!! likewise

    DenseTimeValueModel *input = getInput();
    if (!input) return;

    size_t channelCount = input->getChannelCount();
    if (m_plugin->getMaxChannelCount() < channelCount) {
	channelCount = 1;
    }
    if (m_plugin->getMinChannelCount() > channelCount) {
	std::cerr << "FeatureExtractionPluginTransform:: "
		  << "Can't provide enough channels to plugin (plugin min "
		  << m_plugin->getMinChannelCount() << ", max "
		  << m_plugin->getMaxChannelCount() << ", input model has "
		  << input->getChannelCount() << ")" << std::endl;
	return;
    }

    if (!m_plugin->initialise(channelCount, m_stepSize, m_blockSize)) {
        std::cerr << "FeatureExtractionPluginTransform: Plugin "
                  << m_plugin->getName() << " failed to initialise!" << std::endl;
        return;
    }

    Vamp::Plugin::OutputList outputs = m_plugin->getOutputDescriptors();

    if (outputs.empty()) {
	std::cerr << "FeatureExtractionPluginTransform: Plugin \""
		  << pluginId.toStdString() << "\" has no outputs" << std::endl;
	return;
    }
    
    for (size_t i = 0; i < outputs.size(); ++i) {
	if (outputName == "" || outputs[i].name == outputName.toStdString()) {
	    m_outputFeatureNo = i;
	    m_descriptor = new Vamp::Plugin::OutputDescriptor
		(outputs[i]);
	    break;
	}
    }

    if (!m_descriptor) {
	std::cerr << "FeatureExtractionPluginTransform: Plugin \""
		  << pluginId.toStdString() << "\" has no output named \""
		  << outputName.toStdString() << "\"" << std::endl;
	return;
    }

//    std::cerr << "FeatureExtractionPluginTransform: output sample type "
//	      << m_descriptor->sampleType << std::endl;

    int binCount = 1;
    float minValue = 0.0, maxValue = 0.0;
    
    if (m_descriptor->hasFixedBinCount) {
	binCount = m_descriptor->binCount;
    }

//    std::cerr << "FeatureExtractionPluginTransform: output bin count "
//	      << binCount << std::endl;

    if (binCount > 0 && m_descriptor->hasKnownExtents) {
	minValue = m_descriptor->minValue;
	maxValue = m_descriptor->maxValue;
    }

    size_t modelRate = m_input->getSampleRate();
    size_t modelResolution = 1;
    
    switch (m_descriptor->sampleType) {

    case Vamp::Plugin::OutputDescriptor::VariableSampleRate:
	if (m_descriptor->sampleRate != 0.0) {
	    modelResolution = size_t(modelRate / m_descriptor->sampleRate + 0.001);
	}
	break;

    case Vamp::Plugin::OutputDescriptor::OneSamplePerStep:
	modelResolution = m_stepSize;
	break;

    case Vamp::Plugin::OutputDescriptor::FixedSampleRate:
	modelRate = size_t(m_descriptor->sampleRate + 0.001);
	break;
    }

    if (binCount == 0) {

	m_output = new SparseOneDimensionalModel(modelRate, modelResolution,
						 false);

    } else if (binCount == 1) {

        SparseTimeValueModel *model = new SparseTimeValueModel
            (modelRate, modelResolution, minValue, maxValue, false);
        model->setScaleUnits(outputs[m_outputFeatureNo].unit.c_str());

        m_output = model;

    } else if (m_descriptor->sampleType ==
	       Vamp::Plugin::OutputDescriptor::VariableSampleRate) {

        // We don't have a sparse 3D model, so interpret this as a
        // note model.  There's nothing to define which values to use
        // as which parameters of the note -- for the moment let's
        // treat the first as pitch, second as duration in frames,
        // third (if present) as velocity. (Our note model doesn't
        // yet store velocity.)
        //!!! todo: ask the user!
	
        NoteModel *model = new NoteModel
            (modelRate, modelResolution, minValue, maxValue, false);
        model->setScaleUnits(outputs[m_outputFeatureNo].unit.c_str());

        m_output = model;

    } else {
	
	m_output = new EditableDenseThreeDimensionalModel
            (modelRate, modelResolution, binCount, false);

	if (!m_descriptor->binNames.empty()) {
	    std::vector<QString> names;
	    for (size_t i = 0; i < m_descriptor->binNames.size(); ++i) {
		names.push_back(m_descriptor->binNames[i].c_str());
	    }
	    (dynamic_cast<EditableDenseThreeDimensionalModel *>(m_output))
		->setBinNames(names);
	}
    }
}

FeatureExtractionPluginTransform::~FeatureExtractionPluginTransform()
{
    delete m_plugin;
    delete m_descriptor;
}

DenseTimeValueModel *
FeatureExtractionPluginTransform::getInput()
{
    DenseTimeValueModel *dtvm =
	dynamic_cast<DenseTimeValueModel *>(getInputModel());
    if (!dtvm) {
	std::cerr << "FeatureExtractionPluginTransform::getInput: WARNING: Input model is not conformable to DenseTimeValueModel" << std::endl;
    }
    return dtvm;
}

void
FeatureExtractionPluginTransform::run()
{
    DenseTimeValueModel *input = getInput();
    if (!input) return;

    if (!m_output) return;

    size_t sampleRate = m_input->getSampleRate();

    size_t channelCount = input->getChannelCount();
    if (m_plugin->getMaxChannelCount() < channelCount) {
	channelCount = 1;
    }

    float **buffers = new float*[channelCount];
    for (size_t ch = 0; ch < channelCount; ++ch) {
	buffers[ch] = new float[m_blockSize];
    }

    bool frequencyDomain = (m_plugin->getInputDomain() ==
                            Vamp::Plugin::FrequencyDomain);
    std::vector<FFTModel *> fftModels;

    if (frequencyDomain) {
        for (size_t ch = 0; ch < channelCount; ++ch) {
            fftModels.push_back(new FFTModel
                                  (getInput(),
                                   channelCount == 1 ? m_channel : ch,
                                   HanningWindow,
                                   m_blockSize,
                                   m_stepSize,
                                   m_blockSize,
                                   false));
        }
    }

    long startFrame = m_input->getStartFrame();
    long   endFrame = m_input->getEndFrame();
    long blockFrame = startFrame;

    long prevCompletion = 0;

    while (1) {

        if (frequencyDomain) {
            if (blockFrame - int(m_blockSize)/2 > endFrame) break;
        } else {
            if (blockFrame >= endFrame) break;
        }

//	std::cerr << "FeatureExtractionPluginTransform::run: blockFrame "
//		  << blockFrame << std::endl;

	long completion =
	    (((blockFrame - startFrame) / m_stepSize) * 99) /
	    (   (endFrame - startFrame) / m_stepSize);

	// channelCount is either m_input->channelCount or 1

        for (size_t ch = 0; ch < channelCount; ++ch) {
            if (frequencyDomain) {
                int column = (blockFrame - startFrame) / m_stepSize;
                for (size_t i = 0; i < m_blockSize/2; ++i) {
                    fftModels[ch]->getValuesAt
                        (column, i, buffers[ch][i*2], buffers[ch][i*2+1]);
                }
/*!!!
                float sum = 0.0;
                for (size_t i = 0; i < m_blockSize/2; ++i) {
                    sum += buffers[ch][i*2];
                }
                if (fabs(sum) < 0.0001) {
                    std::cerr << "WARNING: small sum for column " << column << " (sum is " << sum << ")" << std::endl;
                }
*/
            } else {
                getFrames(ch, channelCount, 
                          blockFrame, m_blockSize, buffers[ch]);
            }                
        }

	Vamp::Plugin::FeatureSet features = m_plugin->process
	    (buffers, Vamp::RealTime::frame2RealTime(blockFrame, sampleRate));

	for (size_t fi = 0; fi < features[m_outputFeatureNo].size(); ++fi) {
	    Vamp::Plugin::Feature feature =
		features[m_outputFeatureNo][fi];
	    addFeature(blockFrame, feature);
	}

	if (blockFrame == startFrame || completion > prevCompletion) {
	    setCompletion(completion);
	    prevCompletion = completion;
	}

	blockFrame += m_stepSize;
    }

    Vamp::Plugin::FeatureSet features = m_plugin->getRemainingFeatures();

    for (size_t fi = 0; fi < features[m_outputFeatureNo].size(); ++fi) {
	Vamp::Plugin::Feature feature =
	    features[m_outputFeatureNo][fi];
	addFeature(blockFrame, feature);
    }

    if (frequencyDomain) {
        for (size_t ch = 0; ch < channelCount; ++ch) {
            delete fftModels[ch];
        }
    }

    setCompletion(100);
}

void
FeatureExtractionPluginTransform::getFrames(int channel, int channelCount,
                                            long startFrame, long size,
                                            float *buffer)
{
    long offset = 0;

    if (startFrame < 0) {
        for (int i = 0; i < size && startFrame + i < 0; ++i) {
            buffer[i] = 0.0f;
        }
        offset = -startFrame;
        size -= offset;
        if (size <= 0) return;
        startFrame = 0;
    }

    long got = getInput()->getValues
        ((channelCount == 1 ? m_channel : channel),
         startFrame, startFrame + size, buffer + offset);

    while (got < size) {
        buffer[offset + got] = 0.0;
        ++got;
    }

    if (m_channel == -1 && channelCount == 1 &&
        getInput()->getChannelCount() > 1) {
        // use mean instead of sum, as plugin input
        int cc = getInput()->getChannelCount();
        for (long i = 0; i < size; ++i) {
            buffer[i] /= cc;
        }
    }
}

void
FeatureExtractionPluginTransform::addFeature(size_t blockFrame,
					     const Vamp::Plugin::Feature &feature)
{
    size_t inputRate = m_input->getSampleRate();

//    std::cerr << "FeatureExtractionPluginTransform::addFeature("
//	      << blockFrame << ")" << std::endl;

    int binCount = 1;
    if (m_descriptor->hasFixedBinCount) {
	binCount = m_descriptor->binCount;
    }

    size_t frame = blockFrame;

    if (m_descriptor->sampleType ==
	Vamp::Plugin::OutputDescriptor::VariableSampleRate) {

	if (!feature.hasTimestamp) {
	    std::cerr
		<< "WARNING: FeatureExtractionPluginTransform::addFeature: "
		<< "Feature has variable sample rate but no timestamp!"
		<< std::endl;
	    return;
	} else {
	    frame = Vamp::RealTime::realTime2Frame(feature.timestamp, inputRate);
	}

    } else if (m_descriptor->sampleType ==
	       Vamp::Plugin::OutputDescriptor::FixedSampleRate) {

	if (feature.hasTimestamp) {
	    //!!! warning: sampleRate may be non-integral
	    frame = Vamp::RealTime::realTime2Frame(feature.timestamp,
                                                   m_descriptor->sampleRate);
	} else {
	    frame = m_output->getEndFrame() + 1;
	}
    }
	
    if (binCount == 0) {

	SparseOneDimensionalModel *model = getOutput<SparseOneDimensionalModel>();
	if (!model) return;
	model->addPoint(SparseOneDimensionalModel::Point(frame, feature.label.c_str()));
	
    } else if (binCount == 1) {

	float value = 0.0;
	if (feature.values.size() > 0) value = feature.values[0];

	SparseTimeValueModel *model = getOutput<SparseTimeValueModel>();
	if (!model) return;
	model->addPoint(SparseTimeValueModel::Point(frame, value, feature.label.c_str()));

    } else if (m_descriptor->sampleType == 
	       Vamp::Plugin::OutputDescriptor::VariableSampleRate) {

        float pitch = 0.0;
        if (feature.values.size() > 0) pitch = feature.values[0];

        float duration = 1;
        if (feature.values.size() > 1) duration = feature.values[1];
        
        float velocity = 100;
        if (feature.values.size() > 2) velocity = feature.values[2];

        NoteModel *model = getOutput<NoteModel>();
        if (!model) return;

        model->addPoint(NoteModel::Point(frame, pitch, duration, feature.label.c_str()));
	
    } else {
	
	DenseThreeDimensionalModel::BinValueSet values = feature.values;
	
	EditableDenseThreeDimensionalModel *model =
            getOutput<EditableDenseThreeDimensionalModel>();
	if (!model) return;

	model->setBinValues(frame, values);
    }
}

void
FeatureExtractionPluginTransform::setCompletion(int completion)
{
    int binCount = 1;
    if (m_descriptor->hasFixedBinCount) {
	binCount = m_descriptor->binCount;
    }

    if (binCount == 0) {

	SparseOneDimensionalModel *model = getOutput<SparseOneDimensionalModel>();
	if (!model) return;
	model->setCompletion(completion);

    } else if (binCount == 1) {

	SparseTimeValueModel *model = getOutput<SparseTimeValueModel>();
	if (!model) return;
	model->setCompletion(completion);

    } else if (m_descriptor->sampleType ==
	       Vamp::Plugin::OutputDescriptor::VariableSampleRate) {

	NoteModel *model = getOutput<NoteModel>();
	if (!model) return;
	model->setCompletion(completion);

    } else {

	EditableDenseThreeDimensionalModel *model =
            getOutput<EditableDenseThreeDimensionalModel>();
	if (!model) return;
	model->setCompletion(completion);
    }
}
