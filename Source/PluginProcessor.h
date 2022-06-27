/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <array>

template<typename T>
struct Fifo
{
    void prepare(int numChannels, int numSamples)
    {
        static_assert( std::is_same_v<T, juce::AudioBuffer<float>>,
                      "prepare(numChannels, numSamples) should only be used when the Fifo is holding juce::AudioBuffer<float>");
        for( auto& buffer : buffers)
        {
            buffer.setSize(numChannels,
                           numSamples,
                           false,   //clear everything?
                           true,    //including the extra space?
                           true);   //avoid reallocating if you can?
            buffer.clear();
        }
    }
    
    void prepare(size_t numElements)
    {
        static_assert( std::is_same_v<T, std::vector<float>>,
                      "prepare(numElements) should only be used when the Fifo is holding std::vector<float>");
        for( auto& buffer : buffers )
        {
            buffer.clear();
            buffer.resize(numElements, 0);
        }
    }
    
    bool push(const T& t)
    {
        auto write = fifo.write(1);
        if( write.blockSize1 > 0 )
        {
            buffers[write.startIndex1] = t;
            return true;
        }
        
        return false;
    }
    
    bool pull(T& t)
    {
        auto read = fifo.read(1);
        if( read.blockSize1 > 0 )
        {
            t = buffers[read.startIndex1];
            return true;
        }
        
        return false;
    }
    
    int getNumAvailableForReading() const
    {
        return fifo.getNumReady();
    }
private:
    static constexpr int Capacity = 30;
    std::array<T, Capacity> buffers;
    juce::AbstractFifo fifo {Capacity};
};

enum Channel
{
    Right, //правый моно канал 0
    Left //левый моноканал 1
};

template<typename BlockType>
struct SingleChannelSampleFifo
{
    SingleChannelSampleFifo(Channel ch) : channelToUse(ch)
    {
        prepared.set(false);
    }
    
    void update(const BlockType& buffer)
    {
        jassert(prepared.get());
        jassert(buffer.getNumChannels() > channelToUse );
        auto* channelPtr = buffer.getReadPointer(channelToUse);
        
        for( int i = 0; i < buffer.getNumSamples(); ++i )
        {
            pushNextSampleIntoFifo(channelPtr[i]);
        }
    }

    void prepare(int bufferSize)
    {
        prepared.set(false);
        size.set(bufferSize);
        
        bufferToFill.setSize(1,             //канал
                             bufferSize,    //кол-во примеров
                             false,         //сохранять ли существующишь контент
                             true,          //очистить лишнее место
                             true);         //избежать перенос данных
        audioBufferFifo.prepare(1, bufferSize);
        fifoIndex = 0;
        prepared.set(true);
    }
    //==============================================================================
    int getNumCompleteBuffersAvailable() const { return audioBufferFifo.getNumAvailableForReading(); }
    bool isPrepared() const { return prepared.get(); }
    int getSize() const { return size.get(); }
    //==============================================================================
    bool getAudioBuffer(BlockType& buf) { return audioBufferFifo.pull(buf); }
private:
    Channel channelToUse;
    int fifoIndex = 0;
    Fifo<BlockType> audioBufferFifo;
    BlockType bufferToFill;
    juce::Atomic<bool> prepared = false;
    juce::Atomic<int> size = 0;
    
    void pushNextSampleIntoFifo(float sample)
    {
        if (fifoIndex == bufferToFill.getNumSamples())
        {
            auto ok = audioBufferFifo.push(bufferToFill);

            juce::ignoreUnused(ok);
            
            fifoIndex = 0;
        }
        
        bufferToFill.setSample(0, fifoIndex, sample);
        ++fifoIndex;
    }
};

/**Класс перечисление спусков которые могут быть у звуковой дорожки,
* они соответствуют 12/24/36/48 дБ/октаву соответственно **/
enum Slope
{
    Slope_12,
    Slope_24,
    Slope_36,
    Slope_48
};


/**Структура содержащая в себе настройки цепочки фильтрации звука**/
struct ChainSettings
{
    float peakFreq { 0 }, peakGainInDecibels{ 0 }, peakQuality {1.f};
    float lowCutFreq { 0 }, highCutFreq { 0 };
    Slope lowCutSlope { Slope::Slope_12 }, highCutSlope { Slope::Slope_12 };
    bool lowCutBypassed { false }, peakBypassed { false }, highCutBypassed { false };
};
//

//Настройка фильрации моноканала
ChainSettings getChainSettings(juce::AudioProcessorValueTreeState& apvts);
using Filter = juce::dsp::IIR::Filter<float>;
using CutFilter = juce::dsp::ProcessorChain<Filter, Filter, Filter, Filter>;
using MonoChain = juce::dsp::ProcessorChain<CutFilter, Filter, CutFilter>;
//

/**Класс перечисления для параметров состояния цепи :
* - Низкие частоты
* - Пиковая частота
* - Высокие частоты
**/
enum ChainPositions
{
    LowCut,
    Peak,
    HighCut
};

//Определенение цункций создания фильтра пиковой частоты и обновление коэфффициентов
//Добавление элементов CoefficientsPtr из класса Filter
using Coefficients = Filter::CoefficientsPtr;
void updateCoefficients(Coefficients& old, const Coefficients& replacements);
Coefficients makePeakFilter(const ChainSettings& chainSettings, double sampleRate);


//Шаблонная функция позволяющая обновить информацию о коффициентах цепи
template<int Index, typename ChainType, typename CoefficientType>
void update(ChainType& chain, const CoefficientType& coefficients)
{
    updateCoefficients(chain.template get<Index>().coefficients, coefficients[Index]);
    chain.template setBypassed<Index>(false);
}

//Функция позволяет обновить данные высоких или же низких частот
//В зависимости от передаваемого типа цепи
template<typename ChainType, typename CoefficientType>
void updateCutFilter(ChainType& chain,
                     const CoefficientType& coefficients,
                     const Slope& slope)
{
    chain.template setBypassed<0>(true);
    chain.template setBypassed<1>(true);
    chain.template setBypassed<2>(true);
    chain.template setBypassed<3>(true);
    
    switch( slope )
    {
        case Slope_48:
        {
            update<3>(chain, coefficients);
        }
        case Slope_36:
        {
            update<2>(chain, coefficients);
        }
        case Slope_24:
        {
            update<1>(chain, coefficients);
        }
        case Slope_12:
        {
            update<0>(chain, coefficients);
        }
    }
}


//Встраиваемая(линейная, подставляемая) функция которая создаёт фильтра для низкочастотного диапозона
inline auto makeLowCutFilter(const ChainSettings& chainSettings, double sampleRate )
{
    return juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod(chainSettings.lowCutFreq,
                                                                                       sampleRate,
                                                                               2 * (chainSettings.lowCutSlope + 1));
}

//Встраиваемая(линейная, подставляемая) функция которая создаёт фильтра для высокочастотного диапозона
inline auto makeHighCutFilter(const ChainSettings& chainSettings, double sampleRate )
{ return juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod(chainSettings.highCutFreq,
                                                                                      sampleRate,
                                                                                      2 * (chainSettings.highCutSlope + 1));}
//==============================================================================



//Класс отвечающий за определение аудио-процессора разрабатываемого Простого Эквалайзера
class SimpleEQAudioProcessor  : public juce::AudioProcessor
{
public:
    //Конструктор
    SimpleEQAudioProcessor();
    //Деструктор
    ~SimpleEQAudioProcessor() override;

    //Подготовка к проигрыванию
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    //Очитска ресурсов
    void releaseResources() override;


   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    //Блокировка аудиопотока
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //Создание редактора звука(самого эквалайзера) 
    juce::AudioProcessorEditor* createEditor() override;
    //Создан ли уже редактор
    bool hasEditor() const override;

    //==============================================================================
    //Получение имени продукта(эквалайзера)
    const juce::String getName() const override;

    //учёт средних частот
    bool acceptsMidi() const override;
    //Создание средних частот
    bool producesMidi() const override;
    //Имеется ли эффект создания средних частот
    bool isMidiEffect() const override;

    //Получение времени конечной задержки звука(эхо-эффект)
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;


    //Создание разкладки параметров эквалайзера
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //Дерево состояний значений аудио процессора
    juce::AudioProcessorValueTreeState apvts {*this, nullptr, "Parameters", createParameterLayout()};
    
    using BlockType = juce::AudioBuffer<float>;
    SingleChannelSampleFifo<BlockType> leftChannelFifo { Channel::Left };
    SingleChannelSampleFifo<BlockType> rightChannelFifo { Channel::Right };
private:
    //Левый моноканал, правый моноканал
    MonoChain leftChain, rightChain;
    
    //Обновление фильтра высокой частоты
    void updatePeakFilter(const ChainSettings& chainSettings);

    
    
    //Обновление низкочастотных звуковых фильтров
    void updateLowCutFilters(const ChainSettings& chainSettings);
    //Обновление высокочастотных звуковых фильтров
    void updateHighCutFilters(const ChainSettings& chainSettings);
    //Обновление звуковых фильтров
    void updateFilters();
    
    juce::dsp::Oscillator<float> osc;
    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SimpleEQAudioProcessor)
};
