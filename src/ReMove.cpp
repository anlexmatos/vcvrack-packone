#include "plugin.hpp"
#include "MapModule.hpp"
#include <thread>


const int MAX_DATA = 64 * 1024;
const int MAX_SEQ = 8;

const int RECMODE_TOUCH = 0;
const int RECMODE_MOVE = 1;
const int RECMODE_MANUAL = 2;

const int SEQCHANGEMODE_RESTART = 0;
const int SEQCHANGEMODE_OFFSET = 1;

const int INCVMODE_SOURCE_UNI = 0;
const int INCVMODE_SOURCE_BI = 1;

const int OUTCVMODE_OUT_UNI = 0;
const int OUTCVMODE_OUT_BI = 1;

const int PLAYMODE_LOOP = 0;
const int PLAYMODE_ONESHOT = 1;
const int PLAYMODE_PINGPONG = 2;

const int PLAYDIR_FWD = 1;
const int PLAYDIR_REV = -1;


struct ReMove : MapModule<1> {
    enum ParamIds {
        RUN_PARAM,
        RESET_PARAM,
        REC_PARAM,
        SEQP_PARAM,
        SEQN_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        RUN_INPUT,
        RESET_INPUT,
        PHASE_INPUT,
        SEQ_INPUT,
        CV_INPUT,
        REC_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        CV_OUTPUT,
        REC_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        RUN_LIGHT,
        RESET_LIGHT,
        REC_LIGHT,
        ENUMS(SEQ_LIGHT, 8),
        NUM_LIGHTS
    };

    /** [Stored to JSON] recorded data */
    float *seqData;
    /** stores the current position in data */
    int dataPtr = 0;            

    /** [Stored to JSON] number of sequences */
    int seqCount = 4;
    /** [Stored to JSON] currently selected sequence */
    int seq = 0;
    int seqLow;
    int seqHigh;
    /** [Stored to JSON] length of the seqences */
    int seqLength[MAX_SEQ];

    /** [Stored to JSON] mode for SEQ CV input, 0 = 0-10V, 1 = C4-G4, 2 = Trig */
    int seqCvMode = 0;
    /** [Stored to JSON] behaviour when changing sequences during playback */
    int seqChangeMode = SEQCHANGEMODE_RESTART;

    /** [Stored to JSON] usage-mode for IN input */
    int inCvMode = INCVMODE_SOURCE_UNI;
    /** [Stored to JSON] usage-mode for OUT output*/
    int outCvMode = OUTCVMODE_OUT_UNI;

    /** [Stored to JSON] recording mode */
    int recMode = RECMODE_TOUCH;
    bool recTouched = false;
    float recTouch;

    /** [Stored to JSON] sample rate for recording */
    float sampleRate = 1.f/60.f;          
    dsp::Timer sampleTimer;

    /** [Stored to JSON] mode for playback */
    int playMode = PLAYMODE_LOOP;
    int playDir = PLAYDIR_FWD;

    /** [Stored to JSON] state of playback (for button-press manually) */
    bool isPlaying = false;
    bool isRecording = false;

    dsp::SchmittTrigger seqPTrigger;
    dsp::SchmittTrigger seqNTrigger;
    dsp::SchmittTrigger seqCvTrigger;
    dsp::BooleanTrigger runTrigger;
    dsp::SchmittTrigger resetCvTrigger;
    dsp::BooleanTrigger recTrigger;

	dsp::ClockDivider lightDivider;

    /** for remembering the last touched parameter to avoid dynamic casting */
    Widget *lastParamWidget;

    ReMove() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS); 
        configParam(SEQP_PARAM, 0.0f, 1.0f, 0.0f, "Previous sequence");
        configParam(SEQN_PARAM, 0.0f, 1.0f, 0.0f, "Next sequence");
        configParam(RUN_PARAM, 0.0f, 1.0f, 0.0f, "Run");
        configParam(RESET_PARAM, 0.0f, 1.0f, 0.0f, "Reset");
        configParam(REC_PARAM, 0.0f, 1.0f, 0.0f, "Record");

        seqData = new float[MAX_DATA];
        paramHandles[0].color = nvgRGB(0x40, 0xff, 0xff);
        paramHandles[0].text = "ReMove Lite";

		lightDivider.setDivision(1024);
        onReset();
    }

    ~ReMove() {
        delete[] seqData;
    }

    void onReset() override {
        MapModule::onReset();
        isPlaying = false;
        playDir = PLAYDIR_FWD;
        isRecording = false;
        recTouched = false;
        dataPtr = 0;
        sampleTimer.reset();
        seq = 0;
        seqResize(4); 
        valueFilters[0].reset();
    }

    void process(const ProcessArgs &args) override { 
        outputs[REC_OUTPUT].setVoltage(0);

        // Toggle record when button is pressed
        if (recTrigger.process(params[REC_PARAM].getValue() + inputs[REC_INPUT].getVoltage())) {
            isPlaying = false;
            ParamQuantity *paramQuantity = getParamQuantity(0);
            if (paramQuantity != NULL) {
                isRecording ^= true;
                if (isRecording)
                    startRecording();
                else
                    stopRecording();
            }
        }

        if (isRecording) {
            bool doRecord = true;

            if (recMode == RECMODE_TOUCH && !recTouched) {
                // check if mouse has been pressed on parameter
                Widget *w = APP->event->getDraggedWidget();
                if (w != NULL && w != lastParamWidget) {
                    lastParamWidget = w;
                    // it is not a good idea to do dynamic casting in the DSP thread,
                    // so do this only once for each touched widget
                    ParamWidget *pw = dynamic_cast<ParamWidget*>(w);
                    if (pw != NULL && pw->paramQuantity == getParamQuantity(0))
                        recTouched = true;
                    else 
                        doRecord = false;
                }
                else {
                    doRecord = false;
                }
            }

            if (recMode == RECMODE_MOVE && !recTouched) {
                // check if param value has changed
                if (getValue() != recTouch)
                    recTouched = true;
                else
                    doRecord = false;
            }

            if (doRecord) {           
                if (sampleTimer.process(args.sampleTime) > sampleRate) {
                    // check if mouse button has been released
                    if (APP->event->getDraggedWidget() == NULL) {
                        if (recMode == RECMODE_TOUCH) {     
                            stopRecording();
                        }
                        if (recMode == RECMODE_MOVE) {
                            stopRecording();
                            // trim unchanged values from the end
                            int i = seqLow + seqLength[seq] - 1;
                            if (i > seqLow) {
                                float l = seqData[i];
                                while (i > seqLow && l == seqData[i - 1]) i--;
                                seqLength[seq] = i - seqLow;
                            }
                        } 
                    }
                    
                    // are we still recording?
                    if (isRecording) {
                        seqData[dataPtr] = getValue();
                        seqLength[seq]++;
                        dataPtr++;
                        // stop recording when store is full
                        if (dataPtr == seqHigh) {
                            stopRecording();
                        }
                    }

                    sampleTimer.reset();
                }
                outputs[REC_OUTPUT].setVoltage(10);
            }
        }
        else {
            // Move to previous sequence on button-press
            if (seqPTrigger.process(params[SEQP_PARAM].getValue())) {
                seqPrev();
            }

            // Move to next sequence on button-press
            if (seqNTrigger.process(params[SEQN_PARAM].getValue())) {
                seqNext();
            }

            // SEQ#-input
            if (inputs[SEQ_INPUT].isConnected()) {
                switch (seqCvMode) {
                    case 0:     // 0-10V
                        seqSet(floor(rescale(inputs[SEQ_INPUT].getVoltage(), 0.f, 10.f, 0, seqCount)));
                        break;
                    case 1:     // C4-G4
                        seqSet(round(clamp(inputs[SEQ_INPUT].getVoltage() * 12.f, 0.f, MAX_SEQ - 1.f)));
                        break;
                    case 2:     // Trigger
                        if (seqCvTrigger.process(inputs[SEQ_INPUT].getVoltage()))
                            seqNext();
                        break;
                }
            }

            // RESET-input: reset ptr when button is pressed or input is triggered
            if (resetCvTrigger.process(params[RESET_PARAM].getValue() + inputs[RESET_INPUT].getVoltage())) {
                dataPtr = seqLow;
                playDir = PLAYDIR_FWD;
                sampleTimer.reset();
                valueFilters[0].reset();
            }

            // RUN-button: toggle playing when button is pressed
            if (runTrigger.process(params[RUN_PARAM].getValue())) {
                isPlaying ^= true;
                sampleTimer.reset();
            }

            // RUN-input: Set playing when input is high
            if (inputs[RUN_INPUT].isConnected()) {
                isPlaying = (inputs[RUN_INPUT].getVoltage() >= 1.f);
            }

            // PHASE-input: if position-input is connected set the position directly, ignore playing
            if (inputs[PHASE_INPUT].isConnected()) {
                isPlaying = false;
                ParamQuantity *paramQuantity = getParamQuantity(0);
                if (paramQuantity != NULL) {
                    float v = clamp(inputs[PHASE_INPUT].getVoltage(), 0.f, 10.f);
                    dataPtr = floor(rescale(v, 0.f, 10.f, seqLow, seqLow + seqLength[seq] - 1));
                    v = seqData[dataPtr];
                    setValue(v, paramQuantity);
                }     
            }

            if (isPlaying) {
                if (sampleTimer.process(args.sampleTime) > sampleRate) {
                    ParamQuantity *paramQuantity = getParamQuantity(0);
                    if (paramQuantity == NULL)
                        isPlaying = false;

                    // are we still playing?
                    if (isPlaying && seqLength[seq] > 0) {
                        float v = seqData[dataPtr];
                        dataPtr = dataPtr + playDir;
                        v = valueFilters[0].process(args.sampleTime, v);
                        setValue(v, paramQuantity);
                        if (dataPtr == seqLow + seqLength[seq] && playDir == PLAYDIR_FWD) {
                            switch (playMode) {
                                case PLAYMODE_LOOP: 
                                    dataPtr = seqLow; break;
                                case PLAYMODE_ONESHOT:      // stay on last value
                                    dataPtr--; break;                               
                                case PLAYMODE_PINGPONG:     // reverse direction
                                    dataPtr--; playDir = PLAYDIR_REV; break;       
                            }
                        }
                        if (dataPtr == seqLow - 1) {
                            dataPtr++; playDir = PLAYDIR_FWD;
                        }
                    }
                    sampleTimer.reset();
                }
            }
        }

		// Set channel lights infrequently
		if (lightDivider.process()) {
            lights[RUN_LIGHT].setBrightness(isPlaying);
            lights[RESET_LIGHT].setSmoothBrightness(resetCvTrigger.isHigh(), lightDivider.getDivision() * args.sampleTime);
            lights[REC_LIGHT].setBrightness(isRecording);

            for (int i = 0; i < 8; i++) {
                lights[SEQ_LIGHT + i].setBrightness((seq == i ? 0.7f : 0) + (seqCount >= i + 1 ? 0.3f : 0));
            }
        }

        MapModule::process(args);
    }

    inline float getValue() {
        float v;
        if (inCvMode == INCVMODE_SOURCE_UNI && inputs[CV_INPUT].isConnected()) {
            v = rescale(clamp(inputs[CV_INPUT].getVoltage(), 0.f, 10.f), 0.f, 10.f, 0.f, 1.f);
        }
        else if (inCvMode == INCVMODE_SOURCE_BI && inputs[CV_INPUT].isConnected()) {
            v = rescale(clamp(inputs[CV_INPUT].getVoltage(), -5.f, 5.f), -5.f, 5.f, 0.f, 1.f);
        }
        else {
            ParamQuantity *paramQuantity = getParamQuantity(0);
            v = paramQuantity->getScaledValue();
        }
        return v;
    }

    inline void setValue(float v, ParamQuantity *paramQuantity) {
        paramQuantity->setScaledValue(v);
        if (outCvMode == OUTCVMODE_OUT_UNI && outputs[CV_OUTPUT].isConnected()) {
            outputs[CV_OUTPUT].setVoltage(rescale(v, 0.f, 1.f, 0.f, 10.f));
        }
        else if (outCvMode == OUTCVMODE_OUT_BI && outputs[CV_OUTPUT].isConnected()) {
            outputs[CV_OUTPUT].setVoltage(rescale(v, 0.f, 1.f, -5.f, 5.f));
        }
    }

    inline void startRecording() {
        seqLength[seq] = 0;
        dataPtr = seqLow;
        sampleTimer.reset();
        paramHandles[0].color = nvgRGB(0xff, 0x40, 0xff);
        recTouch = getValue();
        recTouched = false;
    }

    inline void stopRecording() {
        isRecording = false;
        dataPtr = seqLow;
        sampleTimer.reset();      
        paramHandles[0].color = nvgRGB(0x40, 0xff, 0xff);
        valueFilters[0].reset();   
    }

    inline void seqNext() {
        seq = (seq + 1) % seqCount;
        seqUpdate();
    }

    inline void seqPrev() {
        seq = (seq - 1 + seqCount) % seqCount;
        seqUpdate();
    }

    inline void seqSet(int c) {
        if (c == seq) return;
        seq = clamp(c, 0, seqCount - 1);
        seqUpdate();
    }

    void seqResize(int c) {
        if (isRecording) return;
        isPlaying = false;
        seq = 0;
        seqCount = c;
        dataPtr = 0;
        for (int i = 0; i < MAX_SEQ; i++) seqLength[i] = 0;        
        seqUpdate();
    }

    inline void seqUpdate() {
        int s = MAX_DATA / seqCount;    
        seqLow = seq * s;
        seqHigh =  (seq + 1) * s;
        switch (seqChangeMode) {
            case SEQCHANGEMODE_RESTART:
                dataPtr = seqLow;
                playDir = PLAYDIR_FWD;
                sampleTimer.reset();
                valueFilters[0].reset();
                break;
            case SEQCHANGEMODE_OFFSET:
                dataPtr = seqLength[seq] > 0 ? seqLow + (dataPtr % s) % seqLength[seq] : seqLow;
                break;
        }
    }


    void clearMap(int id) override {
        onReset();
        MapModule::clearMap(id);
    }

    void enableLearn(int id) override {
        if (isRecording) return;
        MapModule::enableLearn(id);
    }

    json_t *dataToJson() override {
		json_t *rootJ = MapModule::dataToJson();

        int s = MAX_DATA / seqCount;
		json_t *seqDataJ = json_array();
		for (int i = 0; i < seqCount; i++) {
            json_t *seqData1J = json_array();
            float last1 = 100.f, last2 = -100.f;
            for (int j = 0; j < seqLength[i]; j++) {
                if (last1 == last2) {
                    // 2 times same value -> compress!
                    int c = 0;
                    while (seqData[i * s + j] == last1 && j < seqLength[i]) { c++; j++; }
                    json_array_append(seqData1J, json_integer(c));
                    if (j < seqLength[i]) json_array_append(seqData1J, json_real(seqData[i * s + j]));                    
                    last1 = 100.f; last2 = -100.f;
                } 
                else {
                    json_array_append(seqData1J, json_real(seqData[i * s + j]));
                    last2 = last1;
                    last1 = seqData[i * s + j];
                }
            }
			json_array_append(seqDataJ, seqData1J);
		}
		json_object_set_new(rootJ, "seqData", seqDataJ);

		json_t *seqLengthJ = json_array();
		for (int i = 0; i < seqCount; i++) {
			json_t *d = json_integer(seqLength[i]);
			json_array_append(seqLengthJ, d);
		}
		json_object_set_new(rootJ, "seqLength", seqLengthJ);

        json_object_set_new(rootJ, "seqCount", json_integer(seqCount));
        json_object_set_new(rootJ, "seq", json_integer(seq));
        json_object_set_new(rootJ, "seqCvMode", json_integer(seqCvMode));
        json_object_set_new(rootJ, "seqChangeMode", json_integer(seqChangeMode));
        json_object_set_new(rootJ, "inCvMode", json_integer(inCvMode));
        json_object_set_new(rootJ, "outCvMode", json_integer(outCvMode));
        json_object_set_new(rootJ, "recMode", json_integer(recMode));
        json_object_set_new(rootJ, "playMode", json_integer(playMode));
		json_object_set_new(rootJ, "sampleRate", json_real(sampleRate));
        json_object_set_new(rootJ, "isPlaying", json_boolean(isPlaying));

		return rootJ;
	}

 	void dataFromJson(json_t *rootJ) override {
        MapModule::dataFromJson(rootJ);

    	json_t *seqCountJ = json_object_get(rootJ, "seqCount");
		if (seqCountJ) seqCount = json_integer_value(seqCountJ);
    	json_t *seqJ = json_object_get(rootJ, "seq");
		if (seqJ) seq = json_integer_value(seqJ);
        json_t *seqCvModeJ = json_object_get(rootJ, "seqCvMode");
		if (seqCvModeJ) seqCvMode = json_integer_value(seqCvModeJ);
        json_t *seqChangeModeJ = json_object_get(rootJ, "seqChangeMode");
		if (seqChangeModeJ) seqChangeMode = json_integer_value(seqChangeModeJ);
        json_t *inCvModeJ = json_object_get(rootJ, "inCvMode");
		if (inCvModeJ) inCvMode = json_integer_value(inCvModeJ);
        json_t *outCvModeJ = json_object_get(rootJ, "outCvMode");
		if (outCvModeJ) outCvMode = json_integer_value(outCvModeJ); 
    	json_t *recModeJ = json_object_get(rootJ, "recMode");
		if (recModeJ) recMode = json_integer_value(recModeJ);
    	json_t *playModeJ = json_object_get(rootJ, "playMode");
		if (playModeJ) playMode = json_integer_value(playModeJ);
    	json_t *sampleRateJ = json_object_get(rootJ, "sampleRate");
		if (sampleRateJ) sampleRate = json_real_value(sampleRateJ);
    	json_t *isPlayingJ = json_object_get(rootJ, "isPlaying");
		if (isPlayingJ) isPlaying = json_boolean_value(isPlayingJ);

        json_t *seqLengthJ = json_object_get(rootJ, "seqLength");
		if (seqLengthJ) {
			json_t *d;
			size_t i;
			json_array_foreach(seqLengthJ, i, d) {
                if ((int)i >= seqCount) continue;
                seqLength[i] = json_integer_value(d);
			}
		}

        int s = MAX_DATA / seqCount;
        json_t *seqDataJ = json_object_get(rootJ, "seqData");
		if (seqDataJ) {
			json_t *seqData1J, *d;
			size_t i;
			json_array_foreach(seqDataJ, i, seqData1J) {
                if ((int)i >= seqCount) continue;
                size_t j;
                float last1 = 100.f, last2 = -100.f;
                int c = 0;
                json_array_foreach(seqData1J, j, d) {
                    if (c > seqLength[i]) continue;
                    if (last1 == last2) {
                        // we've seen two same values -> decompress!
                        int v = json_integer_value(d);
                        for (int k = 0; k < v; k++) { seqData[i * s + c] = last1; c++; }
                        last1 = 100.f; last2 = -100.f;
                    }
                    else {
                        seqData[i * s + c] = json_real_value(d);
                        last2 = last1;
                        last1 = seqData[i * s + c];
                        c++;
                    }
                }
			}
		}
        seqUpdate();
	}   
};


struct ReMoveDisplay : TransparentWidget {
	ReMove *module;
	std::shared_ptr<Font> font;

	ReMoveDisplay() {
        font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
	}
	
	void draw(NVGcontext *vg) override {
        if (!module) return;
        float maxX = box.size.x;
        float maxY = box.size.y;

		// Draw ref line
		nvgStrokeColor(vg, nvgRGBA(0xff, 0xb0, 0xf3, 0x20));
        nvgBeginPath(vg);
        nvgMoveTo(vg, 0, maxY / 2);
        nvgLineTo(vg, maxX, maxY / 2);
        nvgClosePath(vg);
        nvgStroke(vg);

        int seqPos = module->dataPtr - module->seqLow;

        if (module->isRecording) {
            // Draw text showing remaining time
            float t = ((float)MAX_DATA / (float)module->seqCount - (float)seqPos) * module->sampleRate;
		    nvgFontSize(vg, 11);
		    nvgFontFaceId(vg, font->handle);
		    nvgTextLetterSpacing(vg, -2.2);
		    nvgFillColor(vg, nvgRGBA(0x66, 0x66, 0x66, 0xff));	
		    nvgTextBox(vg, 6, box.size.y - 4, 120, string::f("REC -%.1fs", t).c_str(), NULL);
        }

        int seqLength = module->seqLength[module->seq];
        if (seqLength < 2) return;

        if (!module->isRecording) {
            // Draw play line
            nvgStrokeColor(vg, nvgRGBA(0xff, 0xb0, 0xf3, 0xb0));
            nvgStrokeWidth(vg, 0.7);
            nvgBeginPath(vg);
            nvgMoveTo(vg, seqPos * maxX / seqLength, 0);
            nvgLineTo(vg, seqPos * maxX / seqLength, maxY);
            nvgClosePath(vg);
            nvgStroke(vg);
        }   

		// Draw automation-line
		nvgStrokeColor(vg, nvgRGBA(0xff, 0xd7, 0x14, 0xc0));
		nvgSave(vg);
		Rect b = Rect(Vec(0, 2), Vec(maxX, maxY - 4));
		nvgScissor(vg, b.pos.x, b.pos.y, b.size.x, b.size.y);
		nvgBeginPath(vg);
        int c = std::min(seqLength, 120);
		for (int i = 0; i < c; i++) {
            float x = (float)i / (c - 1);
            float y = module->seqData[module->seqLow + (int)floor(x * (seqLength - 1))] * 0.96f + 0.02f;
			float px = b.pos.x + b.size.x * x;
			float py = b.pos.y + b.size.y * (1.0 - y);
            if (i == 0)
                nvgMoveTo(vg, px, py);
            else
                nvgLineTo(vg, px, py);
		}

		nvgLineCap(vg, NVG_ROUND);
		nvgMiterLimit(vg, 2.0);
		nvgStrokeWidth(vg, 1.1);
		nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
		nvgStroke(vg);			
		nvgResetScissor(vg);
		nvgRestore(vg);	
	}
};



struct SeqCvModeMenuItem : MenuItem {
    struct SeqCvModItem : MenuItem {
        ReMove *module;
        int seqCvMode;

        void onAction(const event::Action &e) override {
            if (module->isRecording) return;
            module->seqCvMode = seqCvMode;
        }

        void step() override {
            rightText = module->seqCvMode == seqCvMode ? "✔" : "";
            MenuItem::step();
        }
    };
    
    ReMove *module;
    Menu *createChildMenu() override {
        Menu *menu = new Menu;
        std::vector<std::string> names = {"0..10V", "C4-G4", "Trigger"};
        for (size_t i = 0; i < names.size(); i++) {
            menu->addChild(construct<SeqCvModItem>(&MenuItem::text, names[i], &SeqCvModItem::module, module, &SeqCvModItem::seqCvMode, i));
        }
        return menu;
    }
};


struct InCvModeMenuItem : MenuItem {
    ReMove *module;

    void onAction(const event::Action &e) override {
        if (module->isRecording) return;
        module->inCvMode = module->inCvMode == INCVMODE_SOURCE_UNI ? INCVMODE_SOURCE_BI : INCVMODE_SOURCE_UNI;
    }

    void step() override {
        rightText = module->inCvMode == INCVMODE_SOURCE_UNI ? "0V..10V" : "-5V..5V";
        MenuItem::step();
    }
};


struct OutCvModeMenuItem : MenuItem {
    ReMove *module;

    void onAction(const event::Action &e) override {
        module->outCvMode = module->outCvMode == OUTCVMODE_OUT_UNI ? OUTCVMODE_OUT_BI : OUTCVMODE_OUT_UNI;
    }

    void step() override {
        rightText = module->outCvMode == OUTCVMODE_OUT_UNI ? "0V..10V" : "-5V..5V";
        MenuItem::step();
    }
};


struct SampleRateMenuItem : MenuItem {
    struct SampleRateItem : MenuItem {
        ReMove *module;
        float sampleRate;

        void onAction(const event::Action &e) override {
            if (module->isRecording) return;
            module->sampleRate = sampleRate;
        }

        void step() override {
            int s1 = MAX_DATA * sampleRate;
            int s2 = s1 / module->seqCount;
            rightText = string::f(((module->sampleRate == sampleRate) ? "✔ %ds / %ds" : "%ds / %ds"), s1, s2);
            MenuItem::step();
        }
    };
    
    ReMove *module;
    Menu *createChildMenu() override {
        Menu *menu = new Menu;
        menu->addChild(construct<SampleRateItem>(&MenuItem::text, "30Hz", &SampleRateItem::module, module, &SampleRateItem::sampleRate, 1.f/30.f));
        menu->addChild(construct<SampleRateItem>(&MenuItem::text, "60Hz", &SampleRateItem::module, module, &SampleRateItem::sampleRate, 1.f/60.f));
        menu->addChild(construct<SampleRateItem>(&MenuItem::text, "100Hz", &SampleRateItem::module, module, &SampleRateItem::sampleRate, 1.f/100.f));
        menu->addChild(construct<SampleRateItem>(&MenuItem::text, "200Hz", &SampleRateItem::module, module, &SampleRateItem::sampleRate, 1.f/200.f));
        menu->addChild(construct<SampleRateItem>(&MenuItem::text, "500Hz", &SampleRateItem::module, module, &SampleRateItem::sampleRate, 1.f/500.f));
        menu->addChild(construct<SampleRateItem>(&MenuItem::text, "1000Hz", &SampleRateItem::module, module, &SampleRateItem::sampleRate, 1.f/1000.f));
        menu->addChild(construct<SampleRateItem>(&MenuItem::text, "2000Hz", &SampleRateItem::module, module, &SampleRateItem::sampleRate, 1.f/2000.f));
        return menu;
    }
};


struct SeqCountMenuItem : MenuItem {
    struct SeqCountItem : MenuItem {
        ReMove *module;
        int seqCount;

        void onAction(const event::Action &e) override {
            if (module->isRecording) return;
            module->seqResize(seqCount);
        }

        void step() override {
            rightText = (module->seqCount == seqCount) ? "✔" : "";
            MenuItem::step();
        }
    };
    
    ReMove *module;
    Menu *createChildMenu() override {
        Menu *menu = new Menu;
        std::vector<std::string> names = {"1", "2", "4", "8"};
        for (size_t i = 0; i < names.size(); i++) {
            menu->addChild(construct<SeqCountItem>(&MenuItem::text, names[i], &SeqCountItem::module, module, &SeqCountItem::seqCount, (int)pow(2, i)));
        }
        return menu;
    }
};


struct SeqChangeModeMenuItem : MenuItem {
    struct SeqChangeModeItem : MenuItem {
        ReMove *module;
        int seqChangeMode;

        void onAction(const event::Action &e) override {
            module->seqChangeMode = seqChangeMode;
        }

        void step() override {
            rightText = (module->seqChangeMode == seqChangeMode) ? "✔" : "";
            MenuItem::step();
        }
    };
    
    ReMove *module;
    Menu *createChildMenu() override {
        Menu *menu = new Menu;
        menu->addChild(construct<SeqChangeModeItem>(&MenuItem::text, "Restart", &SeqChangeModeItem::module, module, &SeqChangeModeItem::seqChangeMode, SEQCHANGEMODE_RESTART));
        menu->addChild(construct<SeqChangeModeItem>(&MenuItem::text, "Offset", &SeqChangeModeItem::module, module, &SeqChangeModeItem::seqChangeMode, SEQCHANGEMODE_OFFSET));
        return menu;
    }
};


struct RecordModeMenuItem : MenuItem {
    struct RecordModeItem : MenuItem {
        ReMove *module;
        int recMode;

        void onAction(const event::Action &e) override {
            if (module->isRecording) return;
            module->recMode = recMode;
        }

        void step() override {
            rightText = (module->recMode == recMode) ? "✔" : "";
            MenuItem::step();
        }
    };
    
    ReMove *module;
    Menu *createChildMenu() override {
        Menu *menu = new Menu;
        menu->addChild(construct<RecordModeItem>(&MenuItem::text, "Touch", &RecordModeItem::module, module, &RecordModeItem::recMode, RECMODE_TOUCH));
        menu->addChild(construct<RecordModeItem>(&MenuItem::text, "Move", &RecordModeItem::module, module, &RecordModeItem::recMode, RECMODE_MOVE));
        menu->addChild(construct<RecordModeItem>(&MenuItem::text, "Manual", &RecordModeItem::module, module, &RecordModeItem::recMode, RECMODE_MANUAL));
        return menu;
    }
};


struct PlayModeMenuItem : MenuItem {
    struct PlayModeItem : MenuItem {
        ReMove *module;
        int playMode;

        void onAction(const event::Action &e) override {
            module->playMode = playMode;
        }

        void step() override {
            rightText = (module->playMode == playMode) ? "✔" : "";
            MenuItem::step();
        }
    };
    
    ReMove *module;
    Menu *createChildMenu() override {
        Menu *menu = new Menu;
        menu->addChild(construct<PlayModeItem>(&MenuItem::text, "Loop", &PlayModeItem::module, module, &PlayModeItem::playMode, PLAYMODE_LOOP));
        menu->addChild(construct<PlayModeItem>(&MenuItem::text, "Oneshot", &PlayModeItem::module, module, &PlayModeItem::playMode, PLAYMODE_ONESHOT));
        menu->addChild(construct<PlayModeItem>(&MenuItem::text, "Ping Pong", &PlayModeItem::module, module, &PlayModeItem::playMode, PLAYMODE_PINGPONG));
        return menu;
    }
};


struct RecButton : SvgSwitch {
    RecButton() {
        momentary = true;
        box.size = Vec(34.f, 34.f);
        addFrame(APP->window->loadSvg(asset::plugin(pluginInstance, "res/RecButton.svg")));
    }
};

struct RecLight : RedLight {
    std::chrono::time_point<std::chrono::system_clock> blink;
    bool op = true;

	RecLight() {
		bgColor = nvgRGB(0x66, 0x66, 0x66);
		box.size = Vec(27.f, 27.f);
        blink = std::chrono::system_clock::now();
	}

    void step() override {
        if (module) {
            auto now = std::chrono::system_clock::now();
            if (now - blink > std::chrono::milliseconds{800}) {
                op = !op;
                blink = now;
            }

            std::vector<float> brightnesses(baseColors.size());
            for (size_t i = 0; i < baseColors.size(); i++) {
                float b = module->lights[firstLightId + i].getBrightness();
                if (b > 0.f) 
                    b = op ? 1.f : 0.6f;
                brightnesses[i] = b;
            }
            setBrightnesses(brightnesses);
        }
    }

    void drawHalo(const DrawArgs &args) override {
	    float radius = box.size.x / 2.0;
	    float oradius = 5 * radius;

	    nvgBeginPath(args.vg);
	    nvgRect(args.vg, radius - oradius, radius - oradius, 2*oradius, 2*oradius);

	    NVGpaint paint;
	    NVGcolor icol = color::mult(color, 0.4);
	    NVGcolor ocol = nvgRGB(0, 0, 0);

	    paint = nvgRadialGradient(args.vg, radius, radius, radius, oradius, icol, ocol);
	    nvgFillPaint(args.vg, paint);
	    nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
	    nvgFill(args.vg);
    }
};


struct ReMoveWidget : ModuleWidget {
    ReMoveWidget(ReMove *module) {	
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/ReMove.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addChild(createLightCentered<TinyLight<GreenLight>>(Vec(19.5f, 113.8f), module, ReMove::SEQ_LIGHT + 0));
        addChild(createLightCentered<TinyLight<GreenLight>>(Vec(26.8f, 113.8f), module, ReMove::SEQ_LIGHT + 1));
        addChild(createLightCentered<TinyLight<GreenLight>>(Vec(34.1f, 113.8f), module, ReMove::SEQ_LIGHT + 2));
        addChild(createLightCentered<TinyLight<GreenLight>>(Vec(41.4f, 113.8f), module, ReMove::SEQ_LIGHT + 3));
        addChild(createLightCentered<TinyLight<GreenLight>>(Vec(48.6f, 113.8f), module, ReMove::SEQ_LIGHT + 4));
        addChild(createLightCentered<TinyLight<GreenLight>>(Vec(55.9f, 113.8f), module, ReMove::SEQ_LIGHT + 5));
        addChild(createLightCentered<TinyLight<GreenLight>>(Vec(63.2f, 113.8f), module, ReMove::SEQ_LIGHT + 6));
        addChild(createLightCentered<TinyLight<GreenLight>>(Vec(70.5f, 113.8f), module, ReMove::SEQ_LIGHT + 7));

        addInput(createInputCentered<PJ301MPort>(Vec(68.7f, 243.3f), module, ReMove::RUN_INPUT));
        addParam(createParamCentered<TL1105>(Vec(45.f, 230.3f), module, ReMove::RUN_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(Vec(76.7f, 260.5f), module, ReMove::RUN_LIGHT));

        addInput(createInputCentered<PJ301MPort>(Vec(21.1f, 243.3f), module, ReMove::RESET_INPUT));
        addParam(createParamCentered<TL1105>(Vec(45.f, 256.3f), module, ReMove::RESET_PARAM));
        addChild(createLightCentered<SmallLight<GreenLight>>(Vec(13.1f, 260.5f), module, ReMove::RESET_LIGHT));

        addInput(createInputCentered<PJ301MPort>(Vec(68.7f, 200.1f), module, ReMove::PHASE_INPUT));

        addInput(createInputCentered<PJ301MPort>(Vec(21.1f, 336.8f), module, ReMove::CV_INPUT));      
        addOutput(createOutputCentered<PJ301MPort>(Vec(68.7f, 336.8f), module, ReMove::CV_OUTPUT));

        addInput(createInputCentered<PJ301MPort>(Vec(21.1f, 294.1f), module, ReMove::REC_INPUT));      
        addOutput(createOutputCentered<PJ301MPort>(Vec(68.7f, 294.1f), module, ReMove::REC_OUTPUT));

        addParam(createParamCentered<RecButton>(Vec(44.8f, 152.9f), module, ReMove::REC_PARAM));
        addChild(createLightCentered<RecLight>(Vec(44.8f, 152.9f), module, ReMove::REC_LIGHT));

        addInput(createInputCentered<PJ301MPort>(Vec(21.1f, 200.1f), module, ReMove::SEQ_INPUT));
        addParam(createParamCentered<TL1105>(Vec(21.1f, 131.9f), module, ReMove::SEQP_PARAM));
        addParam(createParamCentered<TL1105>(Vec(68.7f, 131.9), module, ReMove::SEQN_PARAM));

		MapModuleDisplay<1> *mapWidget = createWidget<MapModuleDisplay<1>>(Vec(6.8f, 36.4f));
		mapWidget->box.size = Vec(76.2f, 23.f);
		mapWidget->setModule(module);
		addChild(mapWidget);

       	ReMoveDisplay *display = new ReMoveDisplay();
		display->module = module;
		display->box.pos = Vec(6.8f, 65.7f);
		display->box.size = Vec(76.2f, 41.6f);
		addChild(display); 
    }

    void appendContextMenu(Menu *menu) override {
        ReMove *module = dynamic_cast<ReMove*>(this->module);
        assert(module);

        struct ManualItem : MenuItem {
            void onAction(const event::Action &e) override {
                std::thread t(system::openBrowser, "https://github.com/stoermelder/vcvrack-packone/blob/v1/docs/ReMove.md");
                t.detach();
            }
        };

        menu->addChild(construct<ManualItem>(&MenuItem::text, "Module Manual"));
        menu->addChild(new MenuSeparator());

        SampleRateMenuItem *sampleRateMenuItem = construct<SampleRateMenuItem>(&MenuItem::text, "Sample Rate", &SampleRateMenuItem::module, module);
        sampleRateMenuItem->rightText = RIGHT_ARROW;
        menu->addChild(sampleRateMenuItem);

        SeqCountMenuItem *seqCountMenuItem = construct<SeqCountMenuItem>(&MenuItem::text, "# of sequences", &SeqCountMenuItem::module, module);
        seqCountMenuItem->rightText = RIGHT_ARROW;
        menu->addChild(seqCountMenuItem);

        SeqChangeModeMenuItem *seqChangeModeMenuItem = construct<SeqChangeModeMenuItem>(&MenuItem::text, "Sequence change mode", &SeqChangeModeMenuItem::module, module);
        seqChangeModeMenuItem->rightText = RIGHT_ARROW;
        menu->addChild(seqChangeModeMenuItem);

        RecordModeMenuItem *recordModeMenuItem = construct<RecordModeMenuItem>(&MenuItem::text, "Record mode", &RecordModeMenuItem::module, module);
        recordModeMenuItem->rightText = RIGHT_ARROW;
        menu->addChild(recordModeMenuItem);

        PlayModeMenuItem *playModeMenuItem = construct<PlayModeMenuItem>(&MenuItem::text, "Play mode", &PlayModeMenuItem::module, module);
        playModeMenuItem->rightText = RIGHT_ARROW;
        menu->addChild(playModeMenuItem);

        menu->addChild(new MenuSeparator());

        SeqCvModeMenuItem *seqCvModeMenuItem = construct<SeqCvModeMenuItem>(&MenuItem::text, "Port SEQ# mode", &SeqCvModeMenuItem::module, module);
        seqCvModeMenuItem->rightText = RIGHT_ARROW;
        menu->addChild(seqCvModeMenuItem);

        InCvModeMenuItem *inCvModeMenuItem = construct<InCvModeMenuItem>(&MenuItem::text, "Port IN voltage", &InCvModeMenuItem::module, module);
        inCvModeMenuItem->rightText = RIGHT_ARROW;
        menu->addChild(inCvModeMenuItem);

        OutCvModeMenuItem *outCvModeMenuItem = construct<OutCvModeMenuItem>(&MenuItem::text, "Port OUT voltage", &OutCvModeMenuItem::module, module);
        outCvModeMenuItem->rightText = RIGHT_ARROW;
        menu->addChild(outCvModeMenuItem);
    }
};


Model *modelReMoveLite = createModel<ReMove, ReMoveWidget>("ReMoveLite");