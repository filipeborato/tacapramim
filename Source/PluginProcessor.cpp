/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
AudioFilePlayerAudioProcessor::AudioFilePlayerAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{
    // Register formats before any potential reader creation on worker threads
    formatManager.registerBasicFormats();
    // Start background thread if used
    directoryScannerBackgroundThread.startThread();
}

AudioFilePlayerAudioProcessor::~AudioFilePlayerAudioProcessor()
{
    // Ensure background thread stops cleanly
    directoryScannerBackgroundThread.stopThread(2000);
}

//==============================================================================
const juce::String AudioFilePlayerAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioFilePlayerAudioProcessor::acceptsMidi() const { return false; }

bool AudioFilePlayerAudioProcessor::producesMidi() const { return false; }

bool AudioFilePlayerAudioProcessor::isMidiEffect() const { return false; }

double AudioFilePlayerAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int AudioFilePlayerAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int AudioFilePlayerAudioProcessor::getCurrentProgram()
{
    return 0;
}

void AudioFilePlayerAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String AudioFilePlayerAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void AudioFilePlayerAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void AudioFilePlayerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Use this method as the place to do any pre-playback
    // initialisation that you need..
    transportSource.prepareToPlay(samplesPerBlock, sampleRate);
}

void AudioFilePlayerAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
    transportSource.releaseResources();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool AudioFilePlayerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void AudioFilePlayerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // In case we have more outputs than inputs, this code clears any output
    // channels that didn't contain input data, (because these aren't
    // guaranteed to be empty - they may contain garbage).
    // This is here to avoid people getting screaming feedback
    // when they first compile a plugin, but obviously you don't need to keep
    // this code if your algorithm always overwrites all the output channels.
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());
    
    // Pull any newly prepared sources from worker thread (no heavy ops).
    // Keep only the most recent one — older pending entries fall into the pool.
    ReferencedTransportSourceData::Ptr ptr;
    while (fifo.pull(ptr)) {}
    if (ptr != nullptr)
        pendingSource = ptr;

    // RT-safe hot swap without allocations or logging.
    // sourceHasChanged is one-shot: set here, cleared by the editor's timer.
    if (pendingSource != nullptr)
    {
        auto oldActive = activeSource;
        transportSource.stop();
        transportSource.setSource(pendingSource->currentAudioFileSource.get(), 0, nullptr,
                                  pendingSource->audioFileSourceSampleRate);
        activeSource = pendingSource;
        pendingSource = nullptr;
        sourceHasChanged.set(true);
        pool.add(oldActive);
    }

    // Handle transport play param via atomic flag, avoid UI-thread transport calls
    if (auto* playParam = apvts.getRawParameterValue("transportPlay"))
    {
        const bool shouldPlay = (*playParam) > 0.5f;
        const bool wasPlaying = transportIsPlaying.get();
        if (shouldPlay != wasPlaying)
        {
            if (shouldPlay) transportSource.start(); else transportSource.stop();
            transportIsPlaying.set(shouldPlay);
        }
    }
    
    AudioSourceChannelInfo asci(&buffer, 0, buffer.getNumSamples());
    transportSource.getNextAudioBlock(asci);

    // Apply pan (equal-power) and gain
    float gain = 1.0f;
    float pan = 0.0f;
    if (auto* g = apvts.getRawParameterValue("gain")) gain = *g;
    if (auto* p = apvts.getRawParameterValue("pan")) pan = *p;

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin(2, buffer.getNumChannels());
    const float angle = (pan + 1.0f) * (juce::MathConstants<float>::pi * 0.25f);
    const float lg = std::cos(angle);
    const float rg = std::sin(angle);

    if (numCh > 0)
        buffer.applyGain(0, 0, numSamples, gain * lg);
    if (numCh > 1)
        buffer.applyGain(1, 0, numSamples, gain * rg);
}

//==============================================================================
bool AudioFilePlayerAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* AudioFilePlayerAudioProcessor::createEditor()
{
    return new AudioFilePlayerAudioProcessorEditor (*this);
}

//==============================================================================
void AudioFilePlayerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (activeSource != nullptr)
        refreshCurrentFileInAPVTS(apvts, activeSource->currentAudioFile);

    juce::MemoryOutputStream mos(destData, true);
    apvts.state.writeToStream(mos);
}

void AudioFilePlayerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
    auto tree = juce::ValueTree::readFromData(data, sizeInBytes);
    if( tree.isValid() )
    {
        apvts.replaceState(tree);
        if( auto url = apvts.state.getProperty("CurrentFile", {});
           url != var() )
        {
            File file( url.toString() );
            jassert(file.existsAsFile());
            if( file.existsAsFile() )
            {
                juce::URL path( file );
                transportSourceCreator.requestTransportForURL(path);
            }
        }
    }
}

AudioProcessorValueTreeState::ParameterLayout AudioFilePlayerAudioProcessor::createParameterLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<juce::AudioParameterFloat>("gain", "Gain",
                                                           juce::NormalisableRange<float>(0.0f, 2.0f, 0.0f, 1.0f), 1.0f));
    layout.add(std::make_unique<juce::AudioParameterFloat>("pan", "Pan",
                                                           juce::NormalisableRange<float>(-1.0f, 1.0f, 0.0f, 1.0f), 0.0f));
    layout.add(std::make_unique<juce::AudioParameterBool>("transportPlay", "Play", false));

    return layout;
}
//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioFilePlayerAudioProcessor();
}
