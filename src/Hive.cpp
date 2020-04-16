/**
 * q = column
 * r = row
 * 										-r
 * 
 * 
 * 									-q		 *		+q
 * 
 * 
 * 												+r 
 * 
 * 
 *  
 * 									  +0 , -1	     +1 , -1
 * 
 * 
 *							    -1 , +0		 q=0 , r=0 	   +1 , +0 
 * 
 * 
 * 									  -1 , +1	     +0 , +1
 */

#include "plugin.hpp"
#include "digital.hpp"
#include "hexgrid.hpp"
#include <random>
#include <initializer_list>


namespace Hive {

enum GRIDSTATE {
	OFF = 0,
	ON = 1,
	RANDOM = 2
};

enum TURNMODE {
	THIRTY = 0,             ///
	SIXTY = 1,				///
	NINETY = 2,				///
	ONETWENTY = 3,			///
	ONEFIFTY = 4,           ///
	ONEEIGHTY = 5			///
};

enum OUTMODE {
	BI_5V = 0,
	UNI_5V = 1,
	UNI_3V = 2,
	UNI_1V = 3
};

enum MODULESTATE {
	GRID = 0,
	EDIT = 1
};

const int MAX_RADIUS = 16;				/// Max of 16 ensures the area of a cell does not shrink beyond that of one in Maze
const int MIN_RADIUS = 1;				///

const float BOX_WIDTH = 262.563f;								///
const float BOX_HEIGHT = 227.f;									///
const Vec ORIGIN = Vec(BOX_WIDTH / 2.f, BOX_HEIGHT / 2.f);		///

struct HiveCell : HexCell {				///
	GRIDSTATE state;
	float cv;

	HiveCell(GRIDSTATE state, float cv) : state(state), cv(cv) {}
	HiveCell() : state(GRIDSTATE::OFF), cv(0.f) {}
};

struct HiveCursor : HexCell {			///
	int startDir;
	int dir;
	RoundAxialVec startPos;
	RoundAxialVec pos;
	TURNMODE turnMode;
	OUTMODE outMode;
	bool ratchetingEnabled;
	float ratchetingProb;
};

template < int RADIUS, int NUM_PORTS >
struct HiveModule : Module {
	enum ParamIds {
		RESET_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		ENUMS(CLK_INPUT, NUM_PORTS),
		ENUMS(RESET_INPUT, NUM_PORTS),
		ENUMS(TURN_INPUT, NUM_PORTS),
		SHIFT_R1_INPUT,						///
		SHIFT_R2_INPUT,						///
		SHIFT_L1_INPUT,						///
		SHIFT_L2_INPUT,						///
		NUM_INPUTS
	};
	enum OutputIds {
		ENUMS(TRIG_OUTPUT, NUM_PORTS),
		ENUMS(CV_OUTPUT, NUM_PORTS),
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(TRIG_LIGHT, NUM_PORTS),
		ENUMS(CV_LIGHT, NUM_PORTS * 2),
		NUM_LIGHTS
	};

	const int numPorts = NUM_PORTS;

	std::default_random_engine randGen{(uint16_t)std::chrono::system_clock::now().time_since_epoch().count()};
	std::geometric_distribution<int>* geoDist[NUM_PORTS] = {};
	
	typedef HexGrid <HiveCell, HiveCursor, NUM_PORTS, RADIUS, POINTY> HIVEGRID;			///

	/** [Stored to JSON] */
	int panelTheme = 0;

	/** [Stored to JSON] */
	HIVEGRID grid = HIVEGRID(4);		///

	/** [Stored to JSON] */
	float sizeFactor = (BOX_HEIGHT / (((2 * grid.usedRadius) * (3.f / 4.f)) + 1)) / 2.f;		///

	/** [Stored to JSON] */
	bool normalizePorts;

	dsp::SchmittTrigger clockTrigger[NUM_PORTS];
	bool clockTrigger0;
	dsp::SchmittTrigger resetTrigger[NUM_PORTS];
	bool resetTrigger0;
	dsp::SchmittTrigger turnTrigger[NUM_PORTS];
	bool turnTrigger0;
	dsp::Timer resetTimer[NUM_PORTS];
	float resetTimer0;
	dsp::PulseGenerator outPulse[NUM_PORTS];
	ClockMultiplier multiplier[NUM_PORTS];

	dsp::SchmittTrigger shiftR1Trigger;			///
	dsp::SchmittTrigger shiftR2Trigger;			///
	dsp::SchmittTrigger shiftL1Trigger;			///
	dsp::SchmittTrigger shiftL2Trigger;			///

	bool active[NUM_PORTS];
	MODULESTATE currentState = MODULESTATE::GRID;
	bool gridDirty = true;

	dsp::ClockDivider lightDivider;


	HiveModule() {
		panelTheme = pluginSettings.panelThemeDefault;
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		lightDivider.setDivision(128);
		onReset();
	}

	~HiveModule() {
		for (int i = 0; i < NUM_PORTS; i++) {
			delete geoDist[i];
		}
	}

	void onReset() override {
		gridClear();
		for (int i = 0; i < NUM_PORTS; i++) {
			grid.cursor[i].pos.q = grid.cursor[i].startPos.q = -grid.usedRadius;								/// SW edge
			grid.cursor[i].pos.r = grid.cursor[i].startPos.r = (grid.usedRadius + 1) / NUM_PORTS * i;			/// Divide across SW edge
			grid.cursor[i].dir = grid.cursor[i].startDir = 1;													/// Start direction One O'Clock
			grid.cursor[i].turnMode = TURNMODE::SIXTY;															/// Start with small turns 
            grid.cursor[i].diagonalState = false;																/// Diagonal movement starts in clockwise direction
			grid.cursor[i].outMode = OUTMODE::UNI_3V;
			resetTimer[i].reset();
			grid.cursor[i].ratchetingEnabled = true;
			ratchetingSetProb(i);
		}

		normalizePorts = true;
		gridDirty = true;
		Module::onReset();
	}

	void process(const ProcessArgs& args) override {
		if (shiftR1Trigger.process(inputs[SHIFT_R1_INPUT].getVoltage()))
			for (int i = 0; i < NUM_PORTS; i++) {
				if (grid.cursor[i].dir % 2 != 0)
					grid.moveCursor(i, (grid.cursor[i].dir + 2) % 12);				///
				else
					grid.moveCursor(i, (grid.cursor[i].dir + 3) % 12);				///
			}
		if (shiftR2Trigger.process(inputs[SHIFT_R2_INPUT].getVoltage()))
			for (int i = 0; i < NUM_PORTS; i++) {
				if (grid.cursor[i].dir % 2 != 0)
					grid.moveCursor(i, (grid.cursor[i].dir + 4) % 12);				///
				else
					grid.moveCursor(i, (grid.cursor[i].dir + 3) % 12);				///
			}
		if (shiftL1Trigger.process(inputs[SHIFT_L1_INPUT].getVoltage()))
			for (int i = 0; i < NUM_PORTS; i++) {
				if (grid.cursor[i].dir % 2 != 0) 
					grid.moveCursor(i, (grid.cursor[i].dir + 10) % 12);				///
				else
					grid.moveCursor(i, (grid.cursor[i].dir + 9) % 12);				///
			}
		if (shiftL1Trigger.process(inputs[SHIFT_L1_INPUT].getVoltage()))
			for (int i = 0; i < NUM_PORTS; i++) {
				if (grid.cursor[i].dir % 2 != 0)
					grid.moveCursor(i, (grid.cursor[i].dir + 8) % 12);				///
				else
					grid.moveCursor(i, (grid.cursor[i].dir + 9) % 12);
			}

		for (int i = 0; i < NUM_PORTS; i++) {
			active[i] = outputs[TRIG_OUTPUT + i].isConnected() || outputs[CV_OUTPUT + i].isConnected();
			bool doPulse = false;

			if (processResetTrigger(i)) {
				grid.cursor[i].pos = grid.cursor[i].startPos;					///
				grid.cursor[i].dir = grid.cursor[i].startDir;					///
				multiplier[i].reset();
			}

			if (processClockTrigger(i, args.sampleTime)) {
				grid.moveCursor(i, grid.cursor[i].dir);							///
				multiplier[i].tick();

				switch (grid.getCell(grid.cursor[i].pos).state) {            	///
					case GRIDSTATE::OFF:
						break;
					case GRIDSTATE::ON:
						doPulse = true;
						break;
					case GRIDSTATE::RANDOM:
						if (grid.cursor[i].ratchetingEnabled) {
							if (geoDist[i])
								multiplier[i].trigger((*geoDist[i])(randGen));
						}
						else {
							doPulse = random::uniform() >= 0.5f;
						}
						break;
				}
			}

			if (processTurnTrigger(i)) {										///
				switch (grid.cursor[i].turnMode) {
					case THIRTY:
						grid.cursor[i].dir = (grid.cursor[i].dir + 1) % 12;
						break;
					case SIXTY:
						grid.cursor[i].dir = (grid.cursor[i].dir + 2) % 12;
						break;
					case NINETY:
						grid.cursor[i].dir = (grid.cursor[i].dir + 3) % 12;
						break;
					case ONETWENTY:
						grid.cursor[i].dir = (grid.cursor[i].dir + 4) % 12;
						break;
					case ONEFIFTY:
						grid.cursor[i].dir = (grid.cursor[i].dir + 5) % 12;
						break;
					case ONEEIGHTY:
						grid.cursor[i].dir = (grid.cursor[i].dir + 6) % 12;
						break;
				}
			}

			float outGate = 0.f;
			float outCv = outputs[CV_OUTPUT + i].getVoltage();

			if (multiplier[i].process() || doPulse) {
				outPulse[i].trigger();
				HiveCell cell = grid.getCell(grid.cursor[i].pos);					///
				switch (grid.cursor[i].outMode) {									///
					case OUTMODE::BI_5V:
						outCv = rescale(cell.cv, 0.f, 1.f, -5.f, 5.f);				///
						break;
					case OUTMODE::UNI_5V:
						outCv = rescale(cell.cv, 0.f, 1.f, 0.f, 5.f);				///
						break;
					case OUTMODE::UNI_3V:
						outCv = rescale(cell.cv, 0.f, 1.f, 0.f, 3.f);				///
						break;
					case OUTMODE::UNI_1V:
						outCv = cell.cv;											///
						break;
				}
			}

			if (outPulse[i].process(args.sampleTime))
				outGate = 10.f;

			outputs[TRIG_OUTPUT + i].setVoltage(outGate);
			outputs[CV_OUTPUT + i].setVoltage(outCv);
		}

		// Set channel lights infrequently
		if (lightDivider.process()) {
			float s = args.sampleTime * lightDivider.division;
			for (int i = 0; i < NUM_PORTS; i++) {
				float l = outputs[TRIG_OUTPUT + i].isConnected() && outputs[TRIG_OUTPUT + i].getVoltage() > 0.f;
				lights[TRIG_LIGHT + i].setSmoothBrightness(l, s);

				float l1 = outputs[CV_OUTPUT + i].getVoltage() * outputs[CV_OUTPUT + i].isConnected();
				float l2 = l1;
				if (l1 > 0.f) l1 = rescale(l1, 0.f, 5.f, 0.f, 1.f);
				lights[CV_LIGHT + i * 2].setSmoothBrightness(l1, s);
				if (l2 < 0.f) l2 = rescale(l2, -5.f, 0.f, 1.f, 0.f);
				lights[CV_LIGHT + i * 2 + 1].setSmoothBrightness(l2, s);
			}
		}
	}

	inline bool processResetTrigger(int port) {
		if (port == 0) {
			resetTrigger0 = resetTrigger[0].process(inputs[RESET_INPUT].getVoltage() + params[RESET_PARAM].getValue());
			if (resetTrigger0) resetTimer[0].reset();
			return resetTrigger0;
		}
		else {
			if (inputs[RESET_INPUT + port].isConnected()) {
				bool r = resetTrigger[port].process(inputs[RESET_INPUT + port].getVoltage() + params[RESET_PARAM].getValue());
				if (r) resetTimer[port].reset();
				return r;
			}
			else {
				return normalizePorts && resetTrigger0;
			}
		}
	}

	inline bool processClockTrigger(int port, float sampleTime) {
		if (port == 0) {
			resetTimer0 = resetTimer[0].process(sampleTime);
			clockTrigger0 = resetTimer0 >= 1e-3f && clockTrigger[0].process(inputs[CLK_INPUT].getVoltage());
			return clockTrigger0;
		}
		else {
			bool r = resetTimer0 >= 1e-3f;
			if (inputs[RESET_INPUT + port].isConnected()) {
				r = resetTimer[port].process(sampleTime) >= 1e-3f;
			}
			if (inputs[CLK_INPUT + port].isConnected()) {
				return r && clockTrigger[port].process(inputs[CLK_INPUT + port].getVoltage());
			}
			else {
				return normalizePorts && clockTrigger0;
			}
		}
	}

	inline bool processTurnTrigger(int port) {
		if (port == 0) {
			turnTrigger0 = turnTrigger[0].process(inputs[TURN_INPUT].getVoltage());
			return turnTrigger0;
		}
		else {
			if (inputs[TURN_INPUT + port].isConnected()) {
				return turnTrigger[port].process(inputs[TURN_INPUT + port].getVoltage());
			}
			else {
				return normalizePorts && turnTrigger0;
			}
		}
	}

	void gridClear() {										///
		HiveCell cell = HiveCell(GRIDSTATE::OFF, 0.f);
		for (int q = -RADIUS; q <= RADIUS; q++) {
			for (int r = -RADIUS; r <= RADIUS; r++) {
				cell.pos = RoundAxialVec(q, r);
				grid.setCell(cell);
			}
		}
		gridDirty = true;
	}

	void gridResize(int radius) {							///
		if (radius == grid.usedRadius) return;
		sizeFactor = (BOX_HEIGHT / (((2 * radius) * (3.f / 4.f)) + 1)) / 2.f;
		grid.setRadius(radius);

		for (int i = 0; i < NUM_PORTS; i++) {
			grid.cursor[i].startPos.q = -grid.usedRadius;								/// SW edge
			grid.cursor[i].startPos.r = (grid.usedRadius + 1) / NUM_PORTS * i;			/// Divide across SW edge

			if (!cellVisible(grid.cursor[i].pos, radius))
				grid.wrapCursor(i);
		}
		gridDirty = true;
	}

	void gridRandomize(bool useRandom = true) {					///
		HiveCell cell;
		for (int q = -RADIUS; q <= RADIUS; q++) {
			for (int r = -RADIUS; r <= RADIUS; r++) {
				if (cellVisible(q, r, RADIUS)) {				///
					float rand = random::uniform();
					if (rand > 0.8f) {
						cell.state = useRandom ? GRIDSTATE::RANDOM : GRIDSTATE::ON;
						cell.cv = random::uniform();
					}
					else if (rand > 0.6f) {
						cell.state = GRIDSTATE::ON;
						cell.cv = random::uniform();
					}
					else {
						cell.state = GRIDSTATE::OFF;
						cell.cv = 0.f;
					}
					cell.pos = RoundAxialVec(q, r);
					grid.setCell(cell);
				}
			}
		}
		gridDirty = true;
	}

	void cellNextState(HiveCell *cell) {						///
		cell->state = (GRIDSTATE)((cell->state + 1) % 3);
		if (cell->state == GRIDSTATE::ON) cell->cv = random::uniform();
		grid.setCell(*cell);
		gridDirty = true;
	}

	void ratchetingSetProb(int id, float prob = 0.35f) {
		auto geoDistOld = geoDist[id];
		geoDist[id] = new std::geometric_distribution<int>(prob);
		if (geoDistOld) delete geoDistOld;
		grid.cursor[id].ratchetingProb = prob;
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		json_object_set_new(rootJ, "panelTheme", json_integer(panelTheme));

		json_t* gridJ = json_array();
		for (int q = 0; q < grid.arraySize; q++) {				///
			for (int r = 0; r < grid.arraySize; r++) {
				json_array_append_new(gridJ, json_integer(grid.cellMap[q][r].state));
			}
		}
		json_object_set_new(rootJ, "grid", gridJ);

	json_t* gridCvJ = json_array();
		for (int q = 0; q < grid.arraySize; q++) {				///
			for (int r = 0; r < grid.arraySize; r++) {
				json_array_append_new(gridCvJ, json_real(grid.cellMap[q][r].cv));
			}
		}
		json_object_set_new(rootJ, "gridCv", gridCvJ);



		json_t* portsJ = json_array();
		for (int i = 0; i < NUM_PORTS; i++) {												///
			json_t* portJ = json_object();
			json_object_set_new(portJ, "qStartPos", json_integer(grid.cursor[i].startPos.q));
			json_object_set_new(portJ, "rStartPos", json_integer(grid.cursor[i].startPos.r));
			json_object_set_new(portJ, "startDir", json_integer(grid.cursor[i].startDir));
			json_object_set_new(portJ, "qPos", json_integer(grid.cursor[i].pos.q));
			json_object_set_new(portJ, "rPos", json_integer(grid.cursor[i].pos.r));
			json_object_set_new(portJ, "dir", json_integer(grid.cursor[i].dir));
			json_object_set_new(portJ, "turnMode", json_integer(grid.cursor[i].turnMode));
			json_object_set_new(portJ, "diagonalState", json_boolean(grid.cursor[i].diagonalState));
			json_object_set_new(portJ, "outMode", json_integer(grid.cursor[i].outMode));
			json_object_set_new(portJ, "ratchetingProb", json_real(grid.cursor[i].ratchetingProb));
			json_object_set_new(portJ, "ratchetingEnabled", json_boolean(grid.cursor[i].ratchetingEnabled));
			json_array_append_new(portsJ, portJ);
		}
		json_object_set_new(rootJ, "ports", portsJ);

		json_object_set_new(rootJ, "usedRadius", json_integer(grid.usedRadius));			///
		json_object_set_new(rootJ, "sizeFactor", json_real(sizeFactor));					///

		json_object_set_new(rootJ, "normalizePorts", json_boolean(normalizePorts));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		panelTheme = json_integer_value(json_object_get(rootJ, "panelTheme"));

		json_t* gridJ = json_object_get(rootJ, "grid");										///
		for (int q = 0; q < grid.arraySize; q++) {											///
			for (int r = 0; r < grid.arraySize; r++) {
				grid.cellMap[q][r].state = (GRIDSTATE)json_integer_value(json_array_get(gridJ, q * grid.arraySize + r));
			}
		}
		
		json_t* gridCvJ = json_object_get(rootJ, "gridCv");									///
		for (int q = 0; q < grid.arraySize; q++) {											///
			for (int r = 0; r < grid.arraySize; r++) {
				grid.cellMap[q][r].cv = json_real_value(json_array_get(gridCvJ, q * grid.arraySize + r));
			}
		}

		json_t* portsJ = json_object_get(rootJ, "ports");
		json_t* portJ;
		size_t portIndex;
		json_array_foreach(portsJ, portIndex, portJ) {															///
			grid.cursor[portIndex].startPos.q = json_integer_value(json_object_get(portJ, "qStartPos"));
			grid.cursor[portIndex].startPos.r = json_integer_value(json_object_get(portJ, "rStartPos"));	
			grid.cursor[portIndex].startDir = json_integer_value(json_object_get(portJ, "startDir"));
			grid.cursor[portIndex].pos.q = json_integer_value(json_object_get(portJ, "qPos"));
			grid.cursor[portIndex].pos.r = json_integer_value(json_object_get(portJ, "rPos"));
			grid.cursor[portIndex].dir = json_integer_value(json_object_get(portJ, "dir"));
			grid.cursor[portIndex].turnMode = (TURNMODE)json_integer_value(json_object_get(portJ, "turnMode"));
			grid.cursor[portIndex].diagonalState = json_boolean_value(json_object_get(portJ, "diagonalState"));
			grid.cursor[portIndex].outMode = (OUTMODE)json_integer_value(json_object_get(portJ, "outMode"));
			grid.cursor[portIndex].ratchetingEnabled = json_boolean_value(json_object_get(portJ, "ratchetingEnabled"));

			json_t* ratchetingProbJ = json_object_get(portJ, "ratchetingProb");
			if (ratchetingProbJ) {
				ratchetingSetProb(portIndex, json_real_value(ratchetingProbJ));
			}
		}

		grid.usedRadius = json_integer_value(json_object_get(rootJ, "usedRadius"));				///
		sizeFactor = json_real_value(json_object_get(rootJ, "sizeFactor"));						///

		json_t* normalizePortsJ = json_object_get(rootJ, "normalizePorts");
		if (normalizePortsJ) normalizePorts = json_boolean_value(normalizePortsJ);

		json_t* ratchetingEnabledJ = json_object_get(rootJ, "ratchetingEnabled");
		json_t* ratchetingProbJ = json_object_get(rootJ, "ratchetingProb");
		if (ratchetingEnabledJ) {
			for (int i = 0; i < NUM_PORTS; i++) {
				grid.cursor[i].ratchetingEnabled = json_boolean_value(ratchetingEnabledJ);
				ratchetingSetProb(i, json_real_value(ratchetingProbJ));
			}
		}

		gridDirty = true;
	}
};


// Context menus

template < typename MODULE >
struct ModuleStateMenuItem : MenuItem {
	MODULE* module;
	
	void onAction(const event::Action &e) override {
		module->currentState = module->currentState == MODULESTATE::GRID ? MODULESTATE::EDIT : MODULESTATE::GRID;
		module->gridDirty = true;
	}
};

template < typename MODULE >
struct GridCellChangeAction : history::ModuleAction {
	HiveCell oldCell, newCell;										///

	GridCellChangeAction() {
		name = "stoermelder HIVE cell";
	}

	void undo() override {
		app::ModuleWidget* mw = APP->scene->rack->getModule(moduleId);
		assert(mw);
		MODULE* m = dynamic_cast<MODULE*>(mw->module);
		assert(m);
		m->grid.setCell(oldCell);									///
		m->gridDirty = true;
	}

	void redo() override {
		app::ModuleWidget* mw = APP->scene->rack->getModule(moduleId);
		assert(mw);
		MODULE* m = dynamic_cast<MODULE*>(mw->module);
		assert(m);
		m->grid.setCell(newCell);									///
		m->gridDirty = true;
	}
};


template < typename MODULE >
struct GridSizeSlider : ui::Slider {
	struct GridSizeQuantity : Quantity {
		MODULE* module;
		float v = -1.f;

		GridSizeQuantity(MODULE* module) {
			this->module = module;
		}
		void setValue(float value) override {
			v = clamp(value, (float)MIN_RADIUS, (float)MAX_RADIUS);			///
			module->gridResize(int(v));
		}
		float getValue() override {
			if (v < 0.f) v = module->grid.usedRadius;						///
			return v;
		}
		float getDefaultValue() override {
			return 4.f;														///
		}
		float getMinValue() override {
			return float(MIN_RADIUS);										///
		}
		float getMaxValue() override {
			return float(MAX_RADIUS);										///
		}
		float getDisplayValue() override {
			return getValue();
		}
		std::string getDisplayValueString() override {
			int i = int(getValue());
			return string::f("%i", i);										///
		}
		void setDisplayValue(float displayValue) override {
			setValue(displayValue);
		}
		std::string getLabel() override {
			return "Dimension";
		}
		std::string getUnit() override {
			return "";
		}
	};

	GridSizeSlider(MODULE* module) {
		quantity = new GridSizeQuantity(module);
	}
	~GridSizeSlider() {
		delete quantity;
	}
	void onDragMove(const event::DragMove& e) override {
		if (quantity) {
			quantity->moveScaledValue(0.002f * e.mouseDelta.x);
		}
	}
};

template < typename MODULE >
struct GridRandomizeMenuItem : MenuItem {
	MODULE* module;
	bool useRandom = true;
	
	void onAction(const event::Action& e) override {
		// history::ModuleChange
		history::ModuleChange* h = new history::ModuleChange;
		h->name = "stoermelder HIVE grid randomize";
		h->moduleId = module->id;
		h->oldModuleJ = module->toJson();

		module->gridRandomize(useRandom);

		h->newModuleJ = module->toJson();
		APP->history->push(h);
	}
};

template < typename MODULE >
struct GridClearMenuItem : MenuItem {
	MODULE* module;
	
	void onAction(const event::Action& e) override {
		history::ModuleChange* h = new history::ModuleChange;
		h->name = "stoermelder HIVE grid clear";
		h->moduleId = module->id;
		h->oldModuleJ = module->toJson();

		module->gridClear();

		h->newModuleJ = module->toJson();
		APP->history->push(h);
	}
};


// Widgets

template < typename MODULE >
struct HiveGridWidget : FramebufferWidget {
	struct HiveGridDrawWidget : OpaqueWidget {
		MODULE* module;
		NVGcolor gridColor = color::WHITE;

		HiveGridDrawWidget(MODULE* module) {
			this->module = module;
		}

		void draw(const Widget::DrawArgs& args) override {			///
			if (!module) return;

			Vec hex;

			// Draw background
			nvgBeginPath(args.vg);
			drawHex(ORIGIN, ORIGIN.x, FLAT, args.vg);
			nvgFillColor(args.vg, nvgRGB(0, 16, 90));
			nvgFill(args.vg);

			// Draw grid
			nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
			nvgStrokeWidth(args.vg, 0.6f);
			nvgBeginPath(args.vg);
			module->grid.drawGrid(module->sizeFactor, ORIGIN, args.vg);
			nvgStrokeColor(args.vg, color::mult(color::WHITE, 0.075f));
			nvgStroke(args.vg);

			// Draw outer edge
			nvgBeginPath(args.vg);
			module->grid.drawGridOutline(module->sizeFactor, ORIGIN, args.vg);
			nvgStrokeWidth(args.vg, 0.7f);
			nvgStrokeColor(args.vg, color::mult(color::WHITE, 0.125f));
			nvgStroke(args.vg);

			// Draw grid cells
			float stroke = 0.7f;
			float onCellSizeFactor = module->sizeFactor - stroke / 2.f;
			float randCellSizeFactor = module->sizeFactor - stroke;
			float sCellSizeFactor = module->sizeFactor / 2.f;

			for (int q = -module->grid.usedRadius; q <= module->grid.usedRadius; q++) {
				for (int r = -module->grid.usedRadius; r <= module->grid.usedRadius; r++) {
					if (cellVisible(q, r, module->grid.usedRadius)) {
						switch (module->grid.getCell(q, r).state) {
							case GRIDSTATE::ON:
								hex = hexToPixel(RoundAxialVec(q, r), module->sizeFactor, POINTY, ORIGIN);
								nvgBeginPath(args.vg);
								drawHex(hex, onCellSizeFactor, POINTY, args.vg);
								nvgFillColor(args.vg, color::mult(gridColor, 0.7f));
								nvgFill(args.vg);
								break;
							case GRIDSTATE::RANDOM:
								hex = hexToPixel(RoundAxialVec(q, r), module->sizeFactor, POINTY, ORIGIN);
								nvgBeginPath(args.vg);
								drawHex(hex, randCellSizeFactor, POINTY, args.vg);
								nvgStrokeWidth(args.vg, stroke);
								nvgStrokeColor(args.vg, color::mult(gridColor, 0.6f));
								nvgStroke(args.vg);

								nvgBeginPath(args.vg);
								drawHex(hex, sCellSizeFactor, POINTY, args.vg);
								nvgFillColor(args.vg, color::mult(gridColor, 0.4f));
								nvgFill(args.vg);
								break;
							case GRIDSTATE::OFF:
								break;
						}
					}
				}
			}
		}
	};

	MODULE* module;
	HiveGridDrawWidget* w;
	
	HiveGridWidget(MODULE* module) {
		this->module = module;
		w = new HiveGridDrawWidget(module);
		addChild(w);
	}

	void step() override{
		if (module && module->gridDirty) {
			FramebufferWidget::dirty = true;
			w->box.size = box.size;
			w->gridColor = module->currentState == MODULESTATE::EDIT ? color::mult(color::WHITE, 0.35f) : color::WHITE;
			module->gridDirty = false;
		}
		FramebufferWidget::step();
	}
};


template < typename MODULE >
struct HiveDrawHelper {
	MODULE* module;

	Vec c;

	NVGcolor colors[4] = { color::YELLOW, color::RED, color::CYAN, color::BLUE };

	void draw(const Widget::DrawArgs& args, Rect box) {										///
		float cursorRadius = (sqrt(3.f) * module->sizeFactor) / 2.f;

		nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
		for (int i = 0; i < module->numPorts; i++) {
			if (module->currentState == MODULESTATE::EDIT || module->active[i]) {
				c = hexToPixel(	module->currentState == MODULESTATE::EDIT ? module->grid.cursor[i].startPos : module->grid.cursor[i].pos, 
								module->sizeFactor, POINTY, ORIGIN);
				// Inner circle
				nvgGlobalCompositeOperation(args.vg, NVG_ATOP);
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, c.x, c.y, cursorRadius * 0.75f);
				nvgFillColor(args.vg, color::mult(colors[i], 0.35f));
				nvgFill(args.vg);
				// Outer cirlce
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, c.x, c.y, cursorRadius - 0.7f);
				nvgStrokeColor(args.vg, color::mult(colors[i], 0.9f));
				nvgStrokeWidth(args.vg, 0.7f);
				nvgStroke(args.vg);
			}
		}
		for (int i = 0; i < module->numPorts; i++) {
			if (module->currentState == MODULESTATE::EDIT || module->active[i]) {
				c = hexToPixel(	module->currentState == MODULESTATE::EDIT ? module->grid.cursor[i].startPos : module->grid.cursor[i].pos, 
								module->sizeFactor, POINTY, ORIGIN);
				// Halo
				NVGpaint paint;
				NVGcolor icol = color::mult(colors[i], 0.25f);
				NVGcolor ocol = nvgRGB(0, 0, 0);
				nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, c.x, c.y, cursorRadius * 1.5f);
				paint = nvgRadialGradient(args.vg, c.x, c.y, cursorRadius, cursorRadius * 1.5f, icol, ocol);
				nvgFillPaint(args.vg, paint);
				nvgFill(args.vg);
			}
		}
	}
};


template < typename MODULE >
struct HiveStartPosEditWidget : OpaqueWidget, HiveDrawHelper<MODULE> {
	MODULE* module;
	std::shared_ptr<Font> font;
	int selectedId = -1;
	math::Vec dragPos;

	HiveStartPosEditWidget(MODULE* module) {
		font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		this->module = module;
		HiveDrawHelper<MODULE>::module = module;
	}

	void draw(const DrawArgs& args) override {											///
		if (module && module->currentState == MODULESTATE::EDIT) {
			NVGcolor c = color::mult(color::WHITE, 0.7f);
			float stroke = 1.f;
			nvgGlobalCompositeOperation(args.vg, NVG_ATOP);

			// Outer border																///
			nvgBeginPath(args.vg);
			drawHex(ORIGIN, ORIGIN.x, FLAT, args.vg);
			nvgStrokeWidth(args.vg, stroke);
			nvgStrokeColor(args.vg, c);
			nvgStroke(args.vg);

			// Draw "EDIT" text
			nvgFontSize(args.vg, 22);
			nvgFontFaceId(args.vg, font->handle);
			nvgTextLetterSpacing(args.vg, -2.2);
			nvgFillColor(args.vg, c);
			nvgTextBox(args.vg, box.size.x - 101.25f, box.size.y - 6.f, 120, "EDIT", NULL);			///

			HiveDrawHelper<MODULE>::draw(args, box);

			float triangleRadius = (sqrt(3.f) * module->sizeFactor) / 2.f * 0.75f;					///

			nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
			for (int i = 0; i < module->numPorts; i++) {
				// Direction triangle
				Vec c = hexToPixel(module->grid.cursor[i].startPos, module->sizeFactor, POINTY, ORIGIN);	
				Vec vertices[3];
				Vec north[3] = {Vec(0, -triangleRadius),
								Vec(triangleRadius, 0),
								Vec(-triangleRadius, 0)};
				Vec east[3] = {	Vec(triangleRadius, 0),
								Vec(0, triangleRadius),
								Vec(0, -triangleRadius)};
				Vec south[3] = {Vec(0, triangleRadius),
								Vec(-triangleRadius, 0),
								Vec(triangleRadius, 0)};
				Vec west[3] = {	Vec(-triangleRadius, 0),
								Vec(0, -triangleRadius),
								Vec(0, triangleRadius)};
				switch (module->grid.cursor[i].startDir) {
					case 0:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(north[i]);
						break;
					case 1:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(north[i].rotate(M_PI / 6));
						break;
					case 2:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(north[i].rotate(M_PI / 3));
						break;
					case 3:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(east[i]);
						break;
					case 4:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(east[i].rotate(M_PI / 6));
						break;
					case 5:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(east[i].rotate(M_PI / 3));
						break;
					case 6:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(south[i]);
						break;
					case 7:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(south[i].rotate(M_PI / 6));
						break;
					case 8:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(south[i].rotate(M_PI / 3));		
						break;
					case 9:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(west[i]);
						break;
					case 10:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(west[i].rotate(M_PI / 6));
						break;
					case 11:
						for (int i = 0; i < 3; i++)
							vertices[i] = c.plus(west[i].rotate(M_PI / 3));		
						break;
				}
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, vertices[0].x, vertices[0].y);
				nvgLineTo(args.vg, vertices[1].x, vertices[1].y);
				nvgLineTo(args.vg, vertices[2].x, vertices[2].y);
				nvgClosePath(args.vg);
				nvgFillColor(args.vg, color::mult(color::WHITE, 0.9f));
				nvgFill(args.vg);
			}

			OpaqueWidget::draw(args);
		}
	}

	void onButton(const event::Button& e) override {
		if (module && module->currentState == MODULESTATE::EDIT) {
			if (e.action == GLFW_PRESS) {
				selectedId = -1;
				if (gridHovered(e.pos, BOX_WIDTH / 2.f, FLAT, ORIGIN)) {															///
					RoundAxialVec hex = pixelToHex(e.pos, module->sizeFactor, POINTY, ORIGIN);										///
					for (int i = 0; i < module->numPorts; i++) {
						if (module->grid.cursor[i].startPos.q == hex.q && module->grid.cursor[i].startPos.r == hex.r) {				///
							selectedId = i;
							break;
						}
					}

					if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
						dragPos = APP->scene->rack->mousePos.minus(e.pos);
						e.consume(this);
					}
					if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
						if (selectedId == -1) 
							createContextMenu();
						else 
							createDirectionContextMenu();
						e.consume(this);
					}
				}
			} 
			OpaqueWidget::onButton(e);
		}
	}

	void onDragMove(const event::DragMove& e) override {									///
		if (module && module->currentState == MODULESTATE::EDIT) {
			if (e.button != GLFW_MOUSE_BUTTON_LEFT)
				return;
			if (selectedId == -1)
				return;

			math::Vec pos = APP->scene->rack->mousePos.minus(dragPos);
			RoundAxialVec hex = pixelToHex(pos, module->sizeFactor, POINTY, ORIGIN);		///
			if (cellVisible(hex.q, hex.r, module->grid.usedRadius)) {						///
				module->grid.cursor[selectedId].startPos = hex;								///
			}
		}
	}

	void createDirectionContextMenu() {
		ui::Menu* menu = createMenu();

		struct DirectionItem : MenuItem {
			MODULE* module;
			int dir;													///
			int id;

			void onAction(const event::Action &e) override {
				module->grid.cursor[id].startDir = dir;					///
			}

			void step() override {
				bool s = module->grid.cursor[id].startDir == dir;		///
				rightText = s ? "✔" : "";
				MenuItem::step();
			}
		};

		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "Start direction"));
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "12 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 0));		///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "1 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 1));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "2 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 2));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "3 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 3));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "4 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 4));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "5 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 5));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "6 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 6));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "7 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 7));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "8 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 8));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "9 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 9));			///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "10 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 10));		///
		menu->addChild(construct<DirectionItem>(&MenuItem::text, "11 O'Clock", &DirectionItem::module, module, &DirectionItem::id, selectedId, &DirectionItem::dir, 11));		///

		struct TurnModeItem : MenuItem {
			MODULE* module;
			TURNMODE turnMode;
			int id;

			void onAction(const event::Action &e) override {
				module->grid.cursor[id].turnMode = turnMode;
			}

			void step() override {
				rightText = module->grid.cursor[id].turnMode == turnMode ? "✔" : "";
				MenuItem::step();
			}
		};

		menu->addChild(new MenuSeparator());
		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "Turn mode"));
		menu->addChild(construct<TurnModeItem>(&MenuItem::text, "Half", &TurnModeItem::module, module, &TurnModeItem::id, selectedId, &TurnModeItem::turnMode, TURNMODE::THIRTY));					///
		menu->addChild(construct<TurnModeItem>(&MenuItem::text, "Full", &TurnModeItem::module, module, &TurnModeItem::id, selectedId, &TurnModeItem::turnMode, TURNMODE::SIXTY));
		menu->addChild(construct<TurnModeItem>(&MenuItem::text, "Full and Half", &TurnModeItem::module, module, &TurnModeItem::id, selectedId, &TurnModeItem::turnMode, TURNMODE::NINETY));			///
		menu->addChild(construct<TurnModeItem>(&MenuItem::text, "Double", &TurnModeItem::module, module, &TurnModeItem::id, selectedId, &TurnModeItem::turnMode, TURNMODE::ONETWENTY));
		menu->addChild(construct<TurnModeItem>(&MenuItem::text, "Double and Half", &TurnModeItem::module, module, &TurnModeItem::id, selectedId, &TurnModeItem::turnMode, TURNMODE::ONEFIFTY));
		menu->addChild(construct<TurnModeItem>(&MenuItem::text, "Triple", &TurnModeItem::module, module, &TurnModeItem::id, selectedId, &TurnModeItem::turnMode, TURNMODE::ONEEIGHTY));



		struct OutModeItem : MenuItem {
			MODULE* module;
			OUTMODE outMode;
			int id;

			void onAction(const event::Action &e) override {
				module->grid.cursor[id].outMode = outMode;									///
			}

			void step() override {
				rightText = module->grid.cursor[id].outMode == outMode ? "✔" : "";			///
				MenuItem::step();
			}
		};

		menu->addChild(new MenuSeparator());
		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "CV mode"));
		menu->addChild(construct<OutModeItem>(&MenuItem::text, "-5..5V", &OutModeItem::module, module, &OutModeItem::id, selectedId, &OutModeItem::outMode, OUTMODE::BI_5V));
		menu->addChild(construct<OutModeItem>(&MenuItem::text, "0..5V", &OutModeItem::module, module, &OutModeItem::id, selectedId, &OutModeItem::outMode, OUTMODE::UNI_5V));
		menu->addChild(construct<OutModeItem>(&MenuItem::text, "0..3V", &OutModeItem::module, module, &OutModeItem::id, selectedId, &OutModeItem::outMode, OUTMODE::UNI_3V));
		menu->addChild(construct<OutModeItem>(&MenuItem::text, "0..1V", &OutModeItem::module, module, &OutModeItem::id, selectedId, &OutModeItem::outMode, OUTMODE::UNI_1V));

		struct RatchetingMenuItem : MenuItem {
			MODULE* module;
			int id;

			void onAction(const event::Action& e) override {
				module->grid.cursor[id].ratchetingEnabled ^= true;							///
			}

			void step() override {
				rightText = module->grid.cursor[id].ratchetingEnabled ? "✔" : "";			///
				MenuItem::step();
			}
		};

		struct RatchetingProbMenuItem : MenuItem {
			int id;

			RatchetingProbMenuItem() {
				rightText = RIGHT_ARROW;
			}

			struct RatchetingProbItem : MenuItem {
				MODULE* module;
				float ratchetingProb;
				int id;

				void onAction(const event::Action& e) override {
					module->ratchetingSetProb(id, ratchetingProb);
				}

				void step() override {
					rightText = module->grid.cursor[id].ratchetingProb == ratchetingProb ? "✔" : "";		///
					MenuItem::step();
				}
			};

			MODULE* module;
			Menu* createChildMenu() override {
				Menu* menu = new Menu;
				menu->addChild(construct<RatchetingProbItem>(&MenuItem::text, "50%", &RatchetingProbItem::module, module, &RatchetingProbItem::id, id, &RatchetingProbItem::ratchetingProb, 0.5f));
				menu->addChild(construct<RatchetingProbItem>(&MenuItem::text, "60%", &RatchetingProbItem::module, module, &RatchetingProbItem::id, id, &RatchetingProbItem::ratchetingProb, 0.4f));
				menu->addChild(construct<RatchetingProbItem>(&MenuItem::text, "65%", &RatchetingProbItem::module, module, &RatchetingProbItem::id, id, &RatchetingProbItem::ratchetingProb, 0.35f));
				menu->addChild(construct<RatchetingProbItem>(&MenuItem::text, "70%", &RatchetingProbItem::module, module, &RatchetingProbItem::id, id, &RatchetingProbItem::ratchetingProb, 0.3f));
				menu->addChild(construct<RatchetingProbItem>(&MenuItem::text, "80%", &RatchetingProbItem::module, module, &RatchetingProbItem::id, id, &RatchetingProbItem::ratchetingProb, 0.2f));
				menu->addChild(construct<RatchetingProbItem>(&MenuItem::text, "90%", &RatchetingProbItem::module, module, &RatchetingProbItem::id, id, &RatchetingProbItem::ratchetingProb, 0.1f));
				return menu;
			}
		};

		menu->addChild(new MenuSeparator());
		menu->addChild(construct<RatchetingMenuItem>(&MenuItem::text, "Ratcheting", &RatchetingMenuItem::module, module, &RatchetingMenuItem::id, selectedId));
		menu->addChild(construct<RatchetingProbMenuItem>(&MenuItem::text, "Ratcheting probability", &RatchetingProbMenuItem::module, module, &RatchetingProbMenuItem::id, selectedId));
	}

	void createContextMenu() {
		ui::Menu* menu = createMenu();
		menu->addChild(construct<ModuleStateMenuItem<MODULE>>(&MenuItem::text, "Exit Edit-mode", &ModuleStateMenuItem<MODULE>::module, module));
	}
};


template < typename MODULE, typename CELL>
struct HiveScreenWidget : OpaqueWidget, HiveDrawHelper<MODULE> {
	MODULE* module;

	HiveScreenWidget(MODULE* module) {
		this->module = module;
		HiveDrawHelper<MODULE>::module = module;
	}

	void draw(const DrawArgs& args) override {
		if (module && module->currentState == MODULESTATE::GRID) {
			HiveDrawHelper<MODULE>::draw(args, box);
			OpaqueWidget::draw(args);
		}
	}

	void onButton(const event::Button& e) override {
		if (module && module->currentState == MODULESTATE::GRID) {
			if (gridHovered(e.pos, BOX_WIDTH / 2.f, FLAT, ORIGIN)) {									///
				if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
					RoundAxialVec c = pixelToHex(e.pos, module->sizeFactor, POINTY, ORIGIN);			///

					if (cellVisible(c.q, c.r, module->grid.usedRadius)) {								///
						// History
						GridCellChangeAction<MODULE>* h = new GridCellChangeAction<MODULE>;
						CELL cell = module->grid.getCell(c.q, c.r);
						h->moduleId = module->id;
						h->oldCell = cell;																///

						module->cellNextState(&cell);             										///
						
						h->newCell = cell;         														///
						APP->history->push(h);
					}

					e.consume(this);
				}
				if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
					createContextMenu();
					e.consume(this);
				}
			}
			OpaqueWidget::onButton(e);
		}
	}

	void createContextMenu() {
		ui::Menu* menu = createMenu();
		menu->addChild(construct<ModuleStateMenuItem<MODULE>>(&MenuItem::text, "Enter Edit-mode", &ModuleStateMenuItem<MODULE>::module, module));
		menu->addChild(new MenuSeparator());
		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "Grid"));
		GridSizeSlider<MODULE>* sizeSlider = new GridSizeSlider<MODULE>(module);
		sizeSlider->box.size.x = 200.0;
		menu->addChild(sizeSlider);

		menu->addChild(construct<GridRandomizeMenuItem<MODULE>>(&MenuItem::text, "Randomize", &GridRandomizeMenuItem<MODULE>::module, module));
		menu->addChild(construct<GridRandomizeMenuItem<MODULE>>(&MenuItem::text, "Randomize certainty", &GridRandomizeMenuItem<MODULE>::module, module, &GridRandomizeMenuItem<MODULE>::useRandom, false));
		menu->addChild(construct<GridClearMenuItem<MODULE>>(&MenuItem::text, "Clear", &GridClearMenuItem<MODULE>::module, module));
	}
};


struct HiveWidget : ThemedModuleWidget<HiveModule<MAX_RADIUS, 4>> {			///
	typedef HiveModule<MAX_RADIUS, 4> MODULE;
	HiveWidget(MODULE* module)
		: ThemedModuleWidget<HiveModule<MAX_RADIUS, 4>>(module, "Hive") {
		setModule(module);

		addChild(createWidget<StoermelderBlackScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<StoermelderBlackScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<StoermelderBlackScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<StoermelderBlackScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		HiveGridWidget<MODULE>* gridWidget = new HiveGridWidget<MODULE>(module);
		gridWidget->box.pos = Vec(33.709f, 40.3f);
		gridWidget->box.size = Vec(BOX_WIDTH, BOX_HEIGHT);
		addChild(gridWidget);

		HiveScreenWidget<MODULE, HiveCell>* turnWidget = new HiveScreenWidget<MODULE, HiveCell>(module);
		turnWidget->box.pos = gridWidget->box.pos;
		turnWidget->box.size = gridWidget->box.size;
		addChild(turnWidget);

		HiveStartPosEditWidget<MODULE>* resetEditWidget = new HiveStartPosEditWidget<MODULE>(module);
		resetEditWidget->box.pos = turnWidget->box.pos;
		resetEditWidget->box.size = turnWidget->box.size;
		addChild(resetEditWidget);

		addInput(createInputCentered<StoermelderPort>(Vec(23.8f, 67.047f), module, MODULE::SHIFT_L1_INPUT));				///
		addInput(createInputCentered<StoermelderPort>(Vec(23.8f, 256.0f), module, MODULE::SHIFT_L2_INPUT));					///
		addInput(createInputCentered<StoermelderPort>(Vec(306.2f, 67.047f), module, MODULE::SHIFT_R1_INPUT));				///
		addInput(createInputCentered<StoermelderPort>(Vec(306.2f, 256.0f), module, MODULE::SHIFT_R2_INPUT));				///

		addInput(createInputCentered<StoermelderPort>(Vec(119.4f, 292.2f), module, MODULE::CLK_INPUT + 0));
		addInput(createInputCentered<StoermelderPort>(Vec(119.4f, 327.6f), module, MODULE::CLK_INPUT + 1));
		addInput(createInputCentered<StoermelderPort>(Vec(210.6f, 292.2f), module, MODULE::CLK_INPUT + 2));
		addInput(createInputCentered<StoermelderPort>(Vec(210.6f, 327.6f), module, MODULE::CLK_INPUT + 3));

		addInput(createInputCentered<StoermelderPort>(Vec(146.6f, 292.2f), module, MODULE::RESET_INPUT + 0));
		addInput(createInputCentered<StoermelderPort>(Vec(146.6f, 327.6f), module, MODULE::RESET_INPUT + 1));
		addInput(createInputCentered<StoermelderPort>(Vec(183.4f, 292.2f), module, MODULE::RESET_INPUT + 2));
		addInput(createInputCentered<StoermelderPort>(Vec(183.4f, 327.6f), module, MODULE::RESET_INPUT + 3));

		addInput(createInputCentered<StoermelderPort>(Vec(82.8f, 292.2f), module, MODULE::TURN_INPUT + 0));
		addInput(createInputCentered<StoermelderPort>(Vec(82.8f, 327.6f), module, MODULE::TURN_INPUT + 1));
		addInput(createInputCentered<StoermelderPort>(Vec(247.2f, 292.2f), module, MODULE::TURN_INPUT + 2));
		addInput(createInputCentered<StoermelderPort>(Vec(247.2f, 327.6f), module, MODULE::TURN_INPUT + 3));

		addChild(createLightCentered<StoermelderPortLight<GreenLight>>(Vec(51.9f, 292.2f), module, MODULE::TRIG_LIGHT + 0));
		addOutput(createOutputCentered<StoermelderPort>(Vec(51.9f, 292.2f), module, MODULE::TRIG_OUTPUT + 0));
		addChild(createLightCentered<StoermelderPortLight<GreenLight>>(Vec(51.9f, 327.6f), module, MODULE::TRIG_LIGHT + 1));
		addOutput(createOutputCentered<StoermelderPort>(Vec(51.9f, 327.6f), module, MODULE::TRIG_OUTPUT + 1));
		addChild(createLightCentered<StoermelderPortLight<GreenLight>>(Vec(278.2f, 292.2f), module, MODULE::TRIG_LIGHT + 2));
		addOutput(createOutputCentered<StoermelderPort>(Vec(278.2f, 292.2f), module, MODULE::TRIG_OUTPUT + 2));
		addChild(createLightCentered<StoermelderPortLight<GreenLight>>(Vec(278.2f, 327.6f), module, MODULE::TRIG_LIGHT + 3));
		addOutput(createOutputCentered<StoermelderPort>(Vec(278.2f, 327.6f), module, MODULE::TRIG_OUTPUT + 3));

		addChild(createLightCentered<StoermelderPortLight<GreenRedLight>>(Vec(23.8f, 292.2f), module, MODULE::CV_LIGHT + 0));
		addOutput(createOutputCentered<StoermelderPort>(Vec(23.8f, 292.2f), module, MODULE::CV_OUTPUT + 0));
		addChild(createLightCentered<StoermelderPortLight<GreenRedLight>>(Vec(23.8f, 327.6f), module, MODULE::CV_LIGHT + 2));
		addOutput(createOutputCentered<StoermelderPort>(Vec(23.8f, 327.6f), module, MODULE::CV_OUTPUT + 1));
		addChild(createLightCentered<StoermelderPortLight<GreenRedLight>>(Vec(306.2f, 292.2f), module, MODULE::CV_LIGHT + 4));
		addOutput(createOutputCentered<StoermelderPort>(Vec(306.2f, 292.2f), module, MODULE::CV_OUTPUT + 2));
		addChild(createLightCentered<StoermelderPortLight<GreenRedLight>>(Vec(306.2f, 327.6f), module, MODULE::CV_LIGHT + 6));
		addOutput(createOutputCentered<StoermelderPort>(Vec(306.2f, 327.6f), module, MODULE::CV_OUTPUT + 3));
	}

	void appendContextMenu(Menu* menu) override {
		ThemedModuleWidget<MODULE>::appendContextMenu(menu);
		MODULE* module = dynamic_cast<MODULE*>(this->module);

		struct NormalizePortsItem : MenuItem {
			MODULE* module;
			
			void onAction(const event::Action& e) override {
				module->normalizePorts ^= true;
			}

			void step() override {
				rightText = module->normalizePorts ? "✔" : "";
				MenuItem::step();
			}
		};

		menu->addChild(new MenuSeparator());
		menu->addChild(construct<NormalizePortsItem>(&MenuItem::text, "Normalize inputs to Yellow", &NormalizePortsItem::module, module));
	}
};

} // namespace Hive

Model* modelHive = createModel<Hive::HiveModule<Hive::MAX_RADIUS, 4>, Hive::HiveWidget>("Hive");