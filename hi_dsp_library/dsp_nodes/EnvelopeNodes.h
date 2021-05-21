/*  ===========================================================================
 *
 *   This file is part of HISE.
 *   Copyright 2016 Christoph Hart
 *
 *   HISE is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   HISE is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with HISE.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Commercial licenses for using HISE in an closed source project are
 *   available on request. Please visit the project's website to get more
 *   information about commercial licensing:
 *
 *   http://www.hise.audio/
 *
 *   HISE is based on the JUCE library,
 *   which also must be licensed for commercial applications:
 *
 *   http://www.juce.com
 *
 *   ===========================================================================
 */

#pragma once




namespace scriptnode {
using namespace juce;
using namespace hise;
using namespace snex;
using namespace snex::Types;

namespace envelope
{

namespace pimpl
{
template <typename ParameterType> struct envelope_base: public control::pimpl::parameter_node_base<ParameterType>
{
	envelope_base(const Identifier& id): parameter_node_base<ParameterType>(id) 
	{
		cppgen::CustomNodeProperties::addNodeIdManually(id, PropertyIds::IsPolyphonic);
		cppgen::CustomNodeProperties::addNodeIdManually(id, PropertyIds::IsProcessingHiseEvent);
	}

	virtual ~envelope_base() {};

	template <typename BaseType> void postProcess(BaseType& t, bool wasActive, double lastValue)
	{
		static_assert(std::is_base_of<envelope_base, BaseType>(), "not a base class");

		auto thisActive = t.isActive();

		if (thisActive)
		{
			auto mv = t.getModValue();

			if(mv != lastValue)
				getParameter().call<0>(mv);
		}

		if (thisActive != wasActive)
		{
			getParameter().call<1>((double)(int)thisActive);
			getParameter().call<0>(0.0);
		}
	}

	virtual void initialise(NodeBase* n)
	{
		this->p.initialise(n);

		if constexpr (!ParameterType::isStaticList())
		{
			getParameter().numParameters.storeValue(2, n->getUndoManager());
			getParameter().updateParameterAmount({}, 2);
		}
	}

	static constexpr bool isProcessingHiseEvent() { return true; }

	bool handleKeyEvent(HiseEvent& e, bool& newValue)
	{
		if (e.isNoteOn())
		{
			numKeys++;
			newValue = true;
			return numKeys == 1;
		}
		if (e.isNoteOff())
		{
			newValue = false;
			numKeys = jmax(0, numKeys - 1);
			return numKeys == 0;
		}

		return false;
	}

private:

	int numKeys = 0;

	JUCE_DECLARE_WEAK_REFERENCEABLE(envelope_base);
};

struct ahdsr_base: public mothernode,
				   public data::display_buffer_base<true>
{
	struct AhdsrRingBufferProperties : public SimpleRingBuffer::PropertyObject
	{
		static constexpr int PropertyIndex = 2002;

		int getClassIndex() const override { return PropertyIndex; }

		AhdsrRingBufferProperties(SimpleRingBuffer::WriterBase* b) :
			PropertyObject(b),
			base(getTypedBase<ahdsr_base>())
		{}

		RingBufferComponentBase* createComponent() { return new AhdsrGraph(); }

		bool validateInt(const Identifier& id, int& v) const override;

		void transformReadBuffer(AudioSampleBuffer& b) override
		{
			jassert(b.getNumChannels() == 1);
			jassert(b.getNumSamples() == 9);

			if (base != nullptr)
				FloatVectorOperations::copy(b.getWritePointer(0), base->uiValues, 9);
		}

		WeakReference<ahdsr_base> base;
	};

	enum InternalChains
	{
		AttackTimeChain = 0,
		AttackLevelChain,
		DecayTimeChain,
		SustainLevelChain,
		ReleaseTimeChain,
		numInternalChains
	};

	ahdsr_base();

	virtual ~ahdsr_base() {};

	/** @internal The container for the envelope state. */
	struct state_base
	{
		/** The internal states that this envelope has */
		enum EnvelopeState
		{
			ATTACK, ///< attack phase (isPlaying() returns \c true)
			HOLD, ///< hold phase
			DECAY, ///< decay phase
			SUSTAIN, ///< sustain phase (isPlaying() returns \c true)
			RETRIGGER, ///< retrigger phase (monophonic only)
			RELEASE, ///< attack phase (isPlaying() returns \c true)
			IDLE ///< idle state (isPlaying() returns \c false.
		};
		 
		state_base();;

		/** Calculate the attack rate for the state. If the modulation value is 1.0f, they are simply copied from the envelope. */
		void setAttackRate(float rate);;

		/** Calculate the decay rate for the state. If the modulation value is 1.0f, they are simply copied from the envelope. */
		void setDecayRate(float rate);;

		/** Calculate the release rate for the state. If the modulation value is 1.0f, they are simply copied from the envelope. */
		void setReleaseRate(float rate);;

		float tick();

		float getUIPosition(double delta);

		void refreshAttackTime();
		void refreshDecayTime();
		void refreshReleaseTime();

		const ahdsr_base* envelope = nullptr;

		/// the uptime
		int holdCounter;
		float current_value;

		int leftOverSamplesFromLastBuffer = 0;

		/** The ratio in which the attack time is altered. This is calculated by the internal ModulatorChain attackChain*/
		float modValues[5];

		float attackTime;

		float attackLevel;
		float attackCoef;
		float attackBase;

		float decayTime;
		float decayCoef;
		float decayBase;

		float releaseTime;
		float releaseCoef;
		float releaseBase;
		float release_delta;

		float lastSustainValue;
		bool active = false;

		EnvelopeState current_state;
	};

	void calculateCoefficients(float timeInMilliSeconds, float base, float maximum, float &stateBase, float &stateCoeff) const;

	void setBaseSampleRate(double sr)
	{
		sampleRate = sr;
	}

	void setDisplayValue(int index, float value)
	{
		if (index == 1 || index == 4)
			value = Decibels::gainToDecibels(value);

		if(rb != nullptr)
			rb->getUpdater().sendContentChangeMessage(sendNotificationAsync, index);

		uiValues[index] = value;
	}

	float getSampleRateForCurrentMode() const;

	void refreshUIPath(Path& p, Point<float>& position);

	void registerPropertyObject(SimpleRingBuffer::Ptr rb) override
	{
		rb->registerPropertyObject<AhdsrRingBufferProperties>();
	}

	void setAttackRate(float rate);
	void setDecayRate(float rate);
	void setReleaseRate(float rate);
	void setSustainLevel(float level);
	void setHoldTime(float holdTimeMs);
	void setTargetRatioA(float targetRatio);
	void setTargetRatioDR(float targetRatio);

	float calcCoef(float rate, float targetRatio) const;

	void setAttackCurve(float newValue);
	void setDecayCurve(float newValue);

	double sampleRate = -1.0;
	float inputValue;
	float attack;
	float attackLevel;
	float attackCurve;
	float decayCurve;
	float hold;
	float holdTimeSamples;
	float attackBase;
	float decay;
	float decayCoef;
	float decayBase;
	float targetRatioDR;
	float sustain;
	float release;
	float releaseCoef;
	float releaseBase;
	float release_delta;

	float uiValues[9];

	JUCE_DECLARE_WEAK_REFERENCEABLE(ahdsr_base);
};

struct simple_ar_base : public mothernode,
					    public data::display_buffer_base<true>
{
	struct PropertyObject : public hise::SimpleRingBuffer::PropertyObject
	{
		static constexpr int PropertyIndex = 2001;
		PropertyObject(SimpleRingBuffer::WriterBase* p) :
			SimpleRingBuffer::PropertyObject(p),
			parent(getTypedBase<simple_ar_base>())
		{};

		int getClassIndex() const override { return PropertyIndex; }

		RingBufferComponentBase* createComponent() override { return nullptr; }

		bool validateInt(const Identifier& id, int& v) const override;;
		void transformReadBuffer(AudioSampleBuffer& b) override;

		WeakReference<simple_ar_base> parent;
	};

	virtual ~simple_ar_base() {};

	void registerPropertyObject(SimpleRingBuffer::Ptr rb) override
	{
		rb->registerPropertyObject<PropertyObject>();
	}

	void setDisplayValue(int index, double value);

protected:

	struct State
	{
		State() :
			env(10.0f, 10.0f)
		{};

		EnvelopeFollower::AttackRelease env;
		float targetValue = 0.0f;
		float lastValue = 0.0f;
		bool active = false;
		bool smoothing = false;

		float tick()
		{
			if (!smoothing)
				return targetValue;

			lastValue = env.calculateValue(targetValue);
			smoothing = std::abs(targetValue - lastValue) > 0.0001;
			active = smoothing || targetValue == 1.0;
			return lastValue;
		}

		void setGate(bool on)
		{
			targetValue = on ? 1.0f : 0.0f;
			smoothing = true;
		}
	};

private:

	double uiValues[2];

	JUCE_DECLARE_WEAK_REFERENCEABLE(simple_ar_base);
};

}



template <int NV, typename ParameterType> struct simple_ar_impl: public pimpl::envelope_base<ParameterType>,
															     public pimpl::simple_ar_base
{
	static constexpr int NumVoices = NV;

	enum Parameters
	{
		Attack,
		Release,
		Gate
	};

	DEFINE_PARAMETERS
	{
		DEF_PARAMETER(Attack, simple_ar_impl);
		DEF_PARAMETER(Release, simple_ar_impl);
		DEF_PARAMETER(Gate, simple_ar_impl);
	}
	PARAMETER_MEMBER_FUNCTION;

	static constexpr bool isPolyphonic() { return NumVoices > 1; }

	SET_HISE_NODE_ID("simple_ar");
	SN_GET_SELF_AS_OBJECT(simple_ar_impl);

	simple_ar_impl(): pimpl::envelope_base<ParameterType>(getStaticId()) {}

	void setAttack(double ms)
	{
		setDisplayValue(0, ms);

		for (auto& s : states)
			s.env.setAttackDouble(ms);
	}

	void setRelease(double ms)
	{
		setDisplayValue(1, ms);

		for (auto& s : states)
			s.env.setReleaseDouble(ms);
	}

	void prepare(PrepareSpecs ps)
	{
		states.prepare(ps);

		for (auto& s : states)
			s.env.setSampleRate(ps.sampleRate);

		reset();
	}

	void reset()
	{
		for (auto& s : states)
			s.env.reset();
	}

	void handleHiseEvent(HiseEvent& e)
	{
		if constexpr (isPolyphonic())
		{
			if (e.isNoteOnOrOff())
				setGate(e.isNoteOn() ? 1.0 : 0.0);
		}
		else
		{
			bool value;

			if (handleKeyEvent(e, value))
				setGate(value ? 1.0 : 0.0);
		}
	}

	template <typename FrameDataType> void processFrame(FrameDataType& d)
	{
		auto& s = states.get();

		auto thisActive = s.active;
		auto thisValue = s.lastValue;

		auto modValue = s.tick();

		for (auto& v : d)
			v *= modValue;

		postProcess(*this, thisActive, thisValue);
	}

	template <typename ProcessDataType> void process(ProcessDataType& d)
	{
		auto& s = states.get();

		auto thisActive = s.active;
		auto thisValue = s.lastValue;

		if (d.getNumChannels() == 1)
		{
			for (auto& v : d[0])
				v *= s.tick();
		}
		else
		{
			auto fd = d.as<ProcessData<2>>().toFrameData();

			while (fd.next())
			{
				auto modValue = s.tick();
				for (auto& v : fd)
					v *= modValue;
			}
		}

		postProcess(*this, thisActive, thisValue);
	}

	void setGate(double v)
	{
		setDisplayValue(2, v);

		auto a = v > 0.5;

		for (auto& s : states)
			s.setGate(a);
	}

	double getModValue() const
	{
		return states.get().lastValue;
	}

	bool isActive() const
	{
		return states.get().active;
	}

	void createParameters(ParameterDataList& data)
	{
		{
			DEFINE_PARAMETERDATA(simple_ar_impl, Attack);
			p.setRange({ 0.0, 1000.0, 0.1 });
			p.setSkewForCentre(100.0);
			p.setDefaultValue(10.0);
			data.add(std::move(p));
		}

		{
			DEFINE_PARAMETERDATA(simple_ar_impl, Release);
			p.setRange({ 0.0, 1000.0, 0.1 });
			p.setSkewForCentre(100.0);
			p.setDefaultValue(10.0);
			data.add(std::move(p));
		}

		{
			DEFINE_PARAMETERDATA(simple_ar_impl, Gate);
			p.setRange({ 0.0, 1.0, 1.0 });
			p.setDefaultValue(0.0);
			data.add(std::move(p));
		}
	}

	
	
	PolyData<State, NumVoices> states;
};

template <typename ParameterType> using simple_ar = simple_ar_impl<1, ParameterType>;
template <typename ParameterType> using simple_ar_poly = simple_ar_impl<NUM_POLYPHONIC_VOICES, ParameterType>;

template <int NV, typename ParameterType> struct ahdsr : public pimpl::envelope_base<ParameterType>,
														 public pimpl::ahdsr_base
{
	enum Parameters
	{
		Attack,
		AttackLevel,
		Hold,
		Decay,
		Sustain,
		Release,
		AttackCurve,
		Gate,
		numParameters
	};

	ahdsr():
		pimpl::envelope_base<ParameterType>(getStaticId())
	{
		for (auto& s : states)
		{
			s.envelope = this;

			// This makes it use the mod value...
			s.modValues[ahdsr_base::AttackTimeChain] = 0.5f;
			s.modValues[ahdsr_base::ReleaseTimeChain] = 0.5f;
			s.modValues[ahdsr_base::DecayTimeChain] = 0.5f;
		}
			
	}

	static constexpr int NumVoices = NV;

	SET_HISE_POLY_NODE_ID("ahdsr");
	SN_GET_SELF_AS_OBJECT(ahdsr);

	static constexpr bool isProcessingHiseEvent() { return true; }

	void prepare(PrepareSpecs ps)
	{
		states.prepare(ps);

		setBaseSampleRate(ps.sampleRate);
		ballUpdater.limitFromBlockSizeToFrameRate(ps.sampleRate, ps.blockSize);
	}

	void reset()
	{
		for (state_base& s : states)
			s.current_state = pimpl::ahdsr_base::state_base::IDLE;
	}

	void handleHiseEvent(HiseEvent& e)
	{
		if constexpr (isPolyphonic())
		{
			if (e.isNoteOnOrOff())
				setGate(e.isNoteOn() ? 1.0 : 0.0);
		}
		else
		{
			bool value;

			if (handleKeyEvent(e, value))
				setGate(value ? 1.0 : 0.0);
		}
	}

	template <typename T> void process(T& data)
	{
		auto& s = states.get();

		auto thisActive = s.active;
		auto thisValue = s.current_value;

		if (data.getNumChannels() == 1)
		{
			for (auto& v : data[0])
				v *= s.tick();
		}
		else
		{
			auto fd = data.as<ProcessData<2>>().toFrameData();

			while (fd.next())
			{
				auto modValue = s.tick();
				for (auto& v : fd)
					v *= modValue;
			}
		}

		postProcess(*this, thisActive, thisValue);
		updateBallPosition(data.getNumSamples());
	}

	template <typename T> void processFrame(T& data)
	{
		auto& s = states.get();
		auto thisActive = s.active;
		auto thisValue = s.current_value;

		auto modValue = s.tick();

		for (auto& v : data)
			v *= modValue;

		postProcess(*this, thisActive, thisValue);
		updateBallPosition(1);
	}

	void setGate(double v)
	{
		setParameter<Parameters::Gate>(v);
	}

	void updateBallPosition(int numSamples)
	{
		if (ballUpdater.shouldUpdate(numSamples) && rb != nullptr)
		{
			auto& s = states.get();

			if (s.current_state != lastState)
			{
				lastTimeSamples = 0;
				lastState = s.current_state;
			}

			auto delta = 1000.0 * (double)lastTimeSamples / this->sampleRate;
			auto pos = s.getUIPosition(delta);

			rb->sendDisplayIndexMessage(pos);
		}
		
		lastTimeSamples += numSamples;
	}

	bool isActive() const
	{
		return states.get().active;
	}

	double getModValue() const
	{
		return (double)states.get().current_value;
	}

	template <int P> void setParameter(double value)
	{
		auto v = (float)value;

		setDisplayValue(P, v);

		if (P == Parameters::AttackCurve)
		{
			this->setAttackCurve(v);

			for (auto& s : states)
				s.refreshAttackTime();
		}
		else if (P == Parameters::Hold)
		{
			this->setHoldTime(v);
		}
		else
		{
			for (state_base& s : states)
			{
				switch (P)
				{
				case Parameters::Gate:
				{
					auto on = v > 0.5f;

					if (on)
					{
						if(s.current_state == state_base::IDLE)
							s.current_state = state_base::ATTACK;
						else
							s.current_state = state_base::RETRIGGER;
					}

					if (!on && s.current_state != pimpl::ahdsr_base::state_base::IDLE)
						s.current_state = state_base::RELEASE;

					break;
				}

				case Parameters::Attack:
					// We need to trick it to use the poly state value like this...
					s.setAttackRate(v * 2.0f);
					break;
				case Parameters::AttackLevel:
					s.attackLevel = v;
					s.refreshAttackTime();
					break;
				case Parameters::Decay:
					s.setDecayRate(v * 2.0f);
					break;
				case Parameters::Release:
					s.setReleaseRate(v * 2.0f);
					break;
				case Parameters::Sustain:
					s.modValues[3] = v;
					s.refreshReleaseTime();
					s.refreshDecayTime();
					break;
				}
			}
		}
	}

	FORWARD_PARAMETER_TO_MEMBER(ahdsr);

	void createParameters(ParameterDataList& data)
	{
		NormalisableRange<double> timeRange(0.0, 10000.0, 0.1);
		timeRange.setSkewForCentre(300.0);

		

		{
			parameter::data p("Attack", timeRange);
			p.callback = parameter::inner<ahdsr, Parameters::Attack>(*this);
			p.setDefaultValue(10.0);
			data.add(p);
		}
		
		{
			parameter::data p("AttackLevel", { 0.0, 1.0, 0.001 });
			p.callback = parameter::inner<ahdsr, Parameters::AttackLevel>(*this);
			p.setDefaultValue(1.0);
			data.add(p);
		}

		{
			parameter::data p("Decay", timeRange);
			p.callback = parameter::inner<ahdsr, Parameters::Decay>(*this);
			p.setDefaultValue(300.0);
			data.add(p);
		}

		{
			parameter::data p("Hold", timeRange);
			p.callback = parameter::inner<ahdsr, Parameters::Hold>(*this);
			p.setDefaultValue(20.0);
			data.add(p);
		}

		{
			parameter::data p("Sustain", { 0.0, 1.0, 0.001 });
			p.callback = parameter::inner<ahdsr, Parameters::Sustain>(*this);
			p.setDefaultValue(0.5);
			data.add(p);
		}

		{
			parameter::data p("Release", timeRange);
			p.callback = parameter::inner<ahdsr, Parameters::Release>(*this);
			p.setDefaultValue(20.0);
			data.add(p);
		}

		{
			parameter::data p("AttackCurve", { 0.0, 1.0, 0.01 });
			p.callback = parameter::inner<ahdsr, Parameters::AttackCurve>(*this);
			p.setDefaultValue(0.5);
			data.add(p);
		}

		{
			parameter::data p("Gate", { 0.0, 1.0, 1.0 });
			p.callback = parameter::inner<ahdsr, Parameters::Gate>(*this);
			p.setDefaultValue(0.0);
			data.add(p);
		}
	}

	ExecutionLimiter<DummyCriticalSection> ballUpdater;
	state_base::EnvelopeState lastState = state_base::IDLE;
	int lastTimeSamples = 0;

	PolyData<state_base, NumVoices> states;
};

struct voice_manager_base : public mothernode
{
	struct editor : public Component,
		public PooledUIUpdater::SimpleTimer
	{
		editor(PooledUIUpdater* updater, VoiceResetter* n) :
			SimpleTimer(updater),
			vr(n)
		{
			setSize(100, 32 + 10);
		};

		void timerCallback() override
		{
			auto thisVoice = vr != nullptr ? vr->getNumActiveVoices() : 0;

			if (lastVoiceAmount != thisVoice)
			{
				lastVoiceAmount = thisVoice;
				repaint();
			}
		}

		static Component* createExtraComponent(void* obj, PooledUIUpdater* updater)
		{
			auto t = static_cast<mothernode*>(obj);
			auto t2 = dynamic_cast<voice_manager_base*>(t);

			return new editor(updater, t2->p->getVoiceResetter());
		}

		void mouseUp(const MouseEvent& e) override
		{
			if (vr != nullptr)
				vr->onVoiceReset(true, -1);
		}

		void paint(Graphics& g) override
		{
			auto b = getLocalBounds().toFloat();
			b.removeFromBottom(10);

			ScriptnodeComboBoxLookAndFeel::drawScriptnodeDarkBackground(g, b, true);

			auto alpha = 0.4f;

			if (isMouseOver())
				alpha += 0.1f;

			if (isMouseButtonDown())
				alpha += 0.1f;

			if (lastVoiceAmount != 0)
				alpha += 0.2f;

			g.setColour(Colours::white.withAlpha(alpha));
			g.setFont(GLOBAL_BOLD_FONT());

			String s;
			s << String(lastVoiceAmount) << " active voice";

			if (lastVoiceAmount != 1)
				s << "s";

			g.drawText(s, b, Justification::centred);
		}

		int lastVoiceAmount = 0;
		WeakReference<VoiceResetter> vr;
	};

	virtual ~voice_manager_base() {};

	virtual void prepare(PrepareSpecs ps)
	{
		p = ps.voiceIndex;
	}

	PolyHandler* p = nullptr;
};

template <int NV> struct silent_killer_impl: public voice_manager_base
{
	enum Parameters
	{
		Threshold,
		Active
	};

	static constexpr int NumVoices = NV;

	SET_HISE_POLY_NODE_ID("silent_killer");
	SN_GET_SELF_AS_OBJECT(silent_killer_impl);

	HISE_EMPTY_INITIALISE;
	HISE_EMPTY_MOD;
	HISE_EMPTY_RESET;
	
	DEFINE_PARAMETERS
	{
		DEF_PARAMETER(Threshold, silent_killer_impl);
		DEF_PARAMETER(Active, silent_killer_impl);
	}
	PARAMETER_MEMBER_FUNCTION;

	void prepare(PrepareSpecs ps) override
	{
		p = ps.voiceIndex;
		state.prepare(ps);
	}

	template <typename FrameDataType> void processFrame(FrameDataType& d)
	{
		
	}

	template <typename ProcessDataType> void process(ProcessDataType& d)
	{
		auto& s = state.get();

		if (active && s && activeEvents.isEmpty())
		{
			auto bToLook = d[0];
			auto max = FloatVectorOperations::findMaximum(bToLook.begin(), bToLook.size());
			auto isEmpty = max < threshold;

			if (isEmpty)
			{
				s = false;
				p->sendVoiceResetMessage(false);
			}
		}
	}

	void handleHiseEvent(HiseEvent& e)
	{
		if (e.isNoteOn())
		{
			activeEvents.insertWithoutSearch(e.getEventId());
			state.get() = true;
		}
		if (e.isNoteOff())
			activeEvents.remove(e.getEventId());
	}
	
	void setThreshold(double gainDb)
	{
		threshold = Decibels::decibelsToGain(gainDb);
	}

	void setActive(double a)
	{
		active = a > 0.5;
	}

	void createParameters(ParameterDataList& data)
	{
		{
			DEFINE_PARAMETERDATA(silent_killer_impl, Active);
			p.setRange({ 0.0, 1.0, 1.0 });
			p.setDefaultValue(1.0);
			data.add(std::move(p));
		}

		{
			DEFINE_PARAMETERDATA(silent_killer_impl, Threshold);
			p.setRange({ -120.0, -60, 1.0 });
			p.setDefaultValue(-100.0);
			data.add(std::move(p));
		}
	}

	hise::UnorderedStack<int16, NUM_POLYPHONIC_VOICES> activeEvents;
	PolyData<bool, NumVoices> state;
	bool isEmpty = false;
	bool active = false;
	double threshold;
};

using silent_killer = silent_killer_impl<1>;
using silent_killer_poly = silent_killer_impl<NUM_POLYPHONIC_VOICES>;

struct voice_manager: public voice_manager_base
{
	SET_HISE_NODE_ID("voice_manager");
	SN_GET_SELF_AS_OBJECT(voice_manager);

	static constexpr bool isPolyphonic() { return false; }

	HISE_EMPTY_HANDLE_EVENT;
	HISE_EMPTY_MOD;
	HISE_EMPTY_RESET;
	HISE_EMPTY_PROCESS;
	HISE_EMPTY_PROCESS_SINGLE;
	HISE_EMPTY_INITIALISE;

	template <int P> void setParameter(double v)
	{
		auto voiceIndex = p != nullptr ? p->getVoiceIndex() : -1;

		if (P == 0 && v < 0.5 && voiceIndex != -1)
			p->sendVoiceResetMessage(false);

		if (P == 1 && v < 0.5)
			p->sendVoiceResetMessage(true);
	}

	FORWARD_PARAMETER_TO_MEMBER(voice_manager);

	void createParameters(ParameterDataList& data)
	{
		{
			parameter::data d("Kill Voice", { 0.0, 1.0, 1.0 });
			d.callback = parameter::inner<voice_manager, 0>(*this);
			d.setDefaultValue(1.0f);
			data.add(d);
		}
	}
};

};

}