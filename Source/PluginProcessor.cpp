
#include "PluginProcessor.h"
#include "PluginEditor.h"

//Создание объекта класса SimpleEQAudioProcessor и проверка на стереопоточность!
SimpleEQAudioProcessor::SimpleEQAudioProcessor()
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
}

//Создание деструктора класса
SimpleEQAudioProcessor::~SimpleEQAudioProcessor()
{
}

//Получение названия плагина
const juce::String SimpleEQAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

//принятие MIDI
bool SimpleEQAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

//Создание средних частот
bool SimpleEQAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

//Проверка на эффект от потока входа
bool SimpleEQAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

//Получение информации о задержке конца звуковой дорожки
double SimpleEQAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

//Кол-во использующих программ
int SimpleEQAudioProcessor::getNumPrograms()
{
    return 1;   
}

//Программа которая сейчас использует плагин
int SimpleEQAudioProcessor::getCurrentProgram()
{
    return 0;
}

//Установка выбранной программы
void SimpleEQAudioProcessor::setCurrentProgram (int index)
{
}

//Получение названия использующей программы
const juce::String SimpleEQAudioProcessor::getProgramName (int index)
{
    return {};
}

//Смена имени использующей программы
void SimpleEQAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
//Подготовка к проигрыванию трека
void SimpleEQAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Производит всю. необходимую инициализацию
    // Используется для пред настройки параметров
    
    juce::dsp::ProcessSpec spec;
   
    spec.maximumBlockSize = samplesPerBlock;
    
    spec.numChannels = 1;
    
    spec.sampleRate = sampleRate;
    
    leftChain.prepare(spec);
    rightChain.prepare(spec);
    
    updateFilters();
    
    leftChannelFifo.prepare(samplesPerBlock);
    rightChannelFifo.prepare(samplesPerBlock);
    
    osc.initialise([](float x) { return std::sin(x); });
    
    spec.numChannels = getTotalNumOutputChannels();
    osc.prepare(spec);
    osc.setFrequency(440);
}

//Освобождвет используемые ресурсы
void SimpleEQAudioProcessor::releaseResources()
{
    
}

//Проверка поддержки выкладки
#ifndef JucePlugin_PreferredChannelConfigurations
bool SimpleEQAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // Проверяем поддержку выкладки
    // Причём поддержка только моно или стерео потоков.
    if (
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Проверяет соответствие модели и отображения(view)
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

//Обрабатывающий элемерт(тут и происходит вся магия)
void SimpleEQAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    //Чистим всё, что содержит лишнюю информацию
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    //Обновлеям фильтры
    updateFilters();
    
    //Буфер
    juce::dsp::AudioBlock<float> block(buffer);
    
    //Вытаскиваем из буфера оборачиваем в контекст и обновляем 
    //Потоки(музыкальная информация прогоняется через фильтры)
    auto leftBlock = block.getSingleChannelBlock(0);
    auto rightBlock = block.getSingleChannelBlock(1);
    
    juce::dsp::ProcessContextReplacing<float> leftContext(leftBlock);
    juce::dsp::ProcessContextReplacing<float> rightContext(rightBlock);
    
    leftChain.process(leftContext);
    rightChain.process(rightContext);
    
    leftChannelFifo.update(buffer);
    rightChannelFifo.update(buffer);
    
}

//==============================================================================
//Есть ли редактор на экране
bool SimpleEQAudioProcessor::hasEditor() const
{
    return true;
}

//Создание музыкального редактора
juce::AudioProcessorEditor* SimpleEQAudioProcessor::createEditor(){
    //Передаём ссылка на объект AudioProcessor
    return new SimpleEQAudioProcessor(*this);
}

//==============================================================================
//Получение информации о состоянии блока, где хранятся данные(используется для 
//долгосрочного хранения комплексной информации)
void SimpleEQAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream mos(destData, true);
    apvts.state.writeToStream(mos);
}


// Используется для восстановления сохранённых параметров из памяти
// Которые были ранее созданы с помощью метода getStateInformation
void SimpleEQAudioProcessor::setStateInformation (const void* data, int sizeInBytes){
    auto tree = juce::ValueTree::readFromData(data, sizeInBytes);
    if( tree.isValid() )
    {
        apvts.replaceState(tree);
        updateFilters();
    }
}

//Создание цепи параметров для фильтрации
ChainSettings getChainSettings(juce::AudioProcessorValueTreeState& apvts)
{
    ChainSettings settings;
    
    settings.lowCutFreq = apvts.getRawParameterValue("LowCut Freq")->load();
    settings.highCutFreq = apvts.getRawParameterValue("HighCut Freq")->load();
    settings.peakFreq = apvts.getRawParameterValue("Peak Freq")->load();
    settings.peakGainInDecibels = apvts.getRawParameterValue("Peak Gain")->load();
    settings.peakQuality = apvts.getRawParameterValue("Peak Quality")->load();
    settings.lowCutSlope = static_cast<Slope>(apvts.getRawParameterValue("LowCut Slope")->load());
    settings.highCutSlope = static_cast<Slope>(apvts.getRawParameterValue("HighCut Slope")->load());
    
    settings.lowCutBypassed = apvts.getRawParameterValue("LowCut Bypassed")->load() > 0.5f;
    settings.peakBypassed = apvts.getRawParameterValue("Peak Bypassed")->load() > 0.5f;
    settings.highCutBypassed = apvts.getRawParameterValue("HighCut Bypassed")->load() > 0.5f;
    
    return settings;
}

//Создание коэффициентов(фильтра) для пиковой частоты
Coefficients makePeakFilter(const ChainSettings& chainSettings, double sampleRate)
{
    return juce::dsp::IIR::Coefficients<float>::makePeakFilter(sampleRate,
                                                               chainSettings.peakFreq,
                                                               chainSettings.peakQuality,
                                                                  juce::Decibels::decibelsToGain(chainSettings.peakGainInDecibels));
}

//Обновление  фильтров пиковой частоты
void SimpleEQAudioProcessor::updatePeakFilter(const ChainSettings &chainSettings)
{
    //получение коэффициентов
    auto peakCoefficients = makePeakFilter(chainSettings, getSampleRate());
    
    leftChain.setBypassed<ChainPositions::Peak>(chainSettings.peakBypassed);
    rightChain.setBypassed<ChainPositions::Peak>(chainSettings.peakBypassed);
    
    //Обновление на левом канале
    updateCoefficients(leftChain.get<ChainPositions::Peak>().coefficients, peakCoefficients);
    //Обновление на правом канале
    updateCoefficients(rightChain.get<ChainPositions::Peak>().coefficients, peakCoefficients);
}

//Обновление коэффициентов
void updateCoefficients(Coefficients &old, const Coefficients &replacements)
{
    *old = *replacements;
}

//Обновление низкочастотных фильтров
void SimpleEQAudioProcessor::updateLowCutFilters(const ChainSettings &chainSettings)
{
    //Создание коэффициентов для низкочастотного фильтра
    auto cutCoefficients = makeLowCutFilter(chainSettings, getSampleRate());
    //Левый низкочастотный поток
    auto& leftLowCut = leftChain.get<ChainPositions::LowCut>();
    //Правый низкочастотный поток
    auto& rightLowCut = rightChain.get<ChainPositions::LowCut>();
    
    //Устанавливаем фильтры в соответствующие обрабатывающие цепочки
    leftChain.setBypassed<ChainPositions::LowCut>(chainSettings.lowCutBypassed);
    rightChain.setBypassed<ChainPositions::LowCut>(chainSettings.lowCutBypassed);
    
    //Обновляем фильтры 
    updateCutFilter(rightLowCut, cutCoefficients, chainSettings.lowCutSlope);
    updateCutFilter(leftLowCut, cutCoefficients, chainSettings.lowCutSlope);
}

//Функция обновления частотных фильтров//
void SimpleEQAudioProcessor::updateHighCutFilters(const ChainSettings &chainSettings)
{
    //Создание фильтра высоких частот
    auto highCutCoefficients = makeHighCutFilter(chainSettings, getSampleRate());
    
    //фильтр высоких частот левого потока
    auto& leftHighCut = leftChain.get<ChainPositions::HighCut>();
    //фильтр высоких частот правого потока
    auto& rightHighCut = rightChain.get<ChainPositions::HighCut>();
    
    leftChain.setBypassed<ChainPositions::HighCut>(chainSettings.highCutBypassed);
    rightChain.setBypassed<ChainPositions::HighCut>(chainSettings.highCutBypassed);
    
    updateCutFilter(leftHighCut, highCutCoefficients, chainSettings.highCutSlope);
    updateCutFilter(rightHighCut, highCutCoefficients, chainSettings.highCutSlope);
}

//Функция обновления фильтров
void SimpleEQAudioProcessor::updateFilters()
{
    auto chainSettings = getChainSettings(apvts);
    //Обноаляет фильтр низких частот
    updateLowCutFilters(chainSettings);
    //Обновляет фильтр высоких частот
    updatePeakFilter(chainSettings);
    //Обновляет фильтр высоких частот
    updateHighCutFilters(chainSettings);
}

/**Инициализирует модель редактора и возвращает модель раскладки
* 
* Добавляет в выкладку ползунки для изменения:
* - Низких частот
* - Высоких частот
* - усиление пиковой частоты
* - Пиковое качество
* - А также выключатели для этих параметров
* 
* - Также создаётся список дБ на октаву
**/
juce::AudioProcessorValueTreeState::ParameterLayout SimpleEQAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    
    layout.add(std::make_unique<juce::AudioParameterFloat>("LowCut Freq",
                                                           "LowCut Freq",
                                                           juce::NormalisableRange<float>(20.f, 20000.f, 1.f, 0.25f),
                                                           20.f));
    
    layout.add(std::make_unique<juce::AudioParameterFloat>("HighCut Freq",
                                                           "HighCut Freq",
                                                           juce::NormalisableRange<float>(20.f, 20000.f, 1.f, 0.25f),
                                                           20000.f));
    
    layout.add(std::make_unique<juce::AudioParameterFloat>("Peak Freq",
                                                           "Peak Freq",
                                                           juce::NormalisableRange<float>(20.f, 20000.f, 1.f, 0.25f),
                                                           750.f));
    
    layout.add(std::make_unique<juce::AudioParameterFloat>("Peak Gain",
                                                           "Peak Gain",
                                                           juce::NormalisableRange<float>(-24.f, 24.f, 0.5f, 1.f),
                                                           0.0f));
    
    layout.add(std::make_unique<juce::AudioParameterFloat>("Peak Quality",
                                                           "Peak Quality",
                                                           juce::NormalisableRange<float>(0.1f, 10.f, 0.05f, 1.f),
                                                           1.f));
    
    juce::StringArray stringArray;
    for( int i = 0; i < 4; ++i )
    {
        juce::String str;
        str << (12 + i*12);
        str << " db/Oct";
        stringArray.add(str);
    }
    
    layout.add(std::make_unique<juce::AudioParameterChoice>("LowCut Slope", "LowCut Slope", stringArray, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>("HighCut Slope", "HighCut Slope", stringArray, 0));
    layout.add(std::make_unique<juce::AudioParameterBool>("LowCut Bypassed", "LowCut Bypassed", false));
    layout.add(std::make_unique<juce::AudioParameterBool>("Peak Bypassed", "Peak Bypassed", false));
    layout.add(std::make_unique<juce::AudioParameterBool>("HighCut Bypassed", "HighCut Bypassed", false));
    layout.add(std::make_unique<juce::AudioParameterBool>("Analyzer Enabled", "Analyzer Enabled", true));
    
    return layout;
}

//==============================================================================
// Создаёт новый объект плагина.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SimpleEQAudioProcessor();
}
