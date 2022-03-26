/**********************************************************************

  Audacity: A Digital Audio Editor

  StereoToMono.h

  Lynn Allan

**********************************************************************/

#ifndef __AUDACITY_EFFECT_STEREO_TO_MONO__
#define __AUDACITY_EFFECT_STEREO_TO_MONO__

#include "Effect.h"

class EffectStereoToMono final : public Effect
{
public:
   static const ComponentInterfaceSymbol Symbol;

   EffectStereoToMono();
   virtual ~EffectStereoToMono();

   // ComponentInterface implementation

   ComponentInterfaceSymbol GetSymbol() const override;
   TranslatableString GetDescription() const override;

   // EffectDefinitionInterface implementation

   EffectType GetType() const override;
   bool IsInteractive() const override;

   // EffectProcessor implementation

   unsigned GetAudioInCount() const override;
   unsigned GetAudioOutCount() const override;

   // Effect implementation

   bool Process(EffectSettings &settings) override;
   bool IsHiddenFromMenus() const override;

private:
   // EffectStereoToMono implementation

   bool ProcessOne(sampleCount & curTime, sampleCount totalTime, WaveTrack *left, WaveTrack *right);

};

#endif

