/**********************************************************************

  Audacity: A Digital Audio Editor

  Effect.cpp

  Dominic Mazzoni
  Vaughan Johnson
  Martyn Shaw

*******************************************************************//**

\class Effect
\brief Base class for many of the effects in Audacity.

*//*******************************************************************/


#include "Effect.h"
#include "TimeWarper.h"

#include <algorithm>
#include <thread>

#include <wx/defs.h>
#include <wx/sizer.h>

#include "../AudioIO.h"
#include "widgets/wxWidgetsWindowPlacement.h"
#include "../LabelTrack.h"
#include "../MixAndRender.h"
#include "PluginManager.h"
#include "../ProjectAudioManager.h"
#include "../ProjectSettings.h"
#include "QualitySettings.h"
#include "../SelectFile.h"
#include "../ShuttleAutomation.h"
#include "../ShuttleGui.h"
#include "../SyncLock.h"
#include "TransactionScope.h"
#include "ViewInfo.h"
#include "../WaveTrack.h"
#include "wxFileNameWrapper.h"
#include "../widgets/ProgressDialog.h"
#include "../widgets/NumericTextCtrl.h"
#include "../widgets/AudacityMessageBox.h"
#include "../widgets/VetoDialogHook.h"

#include <unordered_map>

// Effect application counter
int Effect::nEffectsDone=0;

static const int kPlayID = 20102;
static const int kRewindID = 20103;
static const int kFFwdID = 20104;

using t2bHash = std::unordered_map< void*, bool >;

Effect::Effect()
{
   mTracks = NULL;
   mT0 = 0.0;
   mT1 = 0.0;
   mIsPreview = false;
   mIsLinearEffect = false;
   mPreviewWithNotSelected = false;
   mPreviewFullSelection = false;
   mNumTracks = 0;
   mNumGroups = 0;
   mProgress = NULL;

   mUIParent = NULL;

   mBufferSize = 0;
   mBlockSize = 0;
   mNumChannels = 0;

   // PRL:  I think this initialization of mProjectRate doesn't matter
   // because it is always reassigned in DoEffect before it is used
   // STF: but can't call AudioIOBase::GetOptimalSupportedSampleRate() here.
   // (Which is called to compute the default-default value.)  (Bug 2280)
   mProjectRate = QualitySettings::DefaultSampleRate.ReadWithDefault(44100);

   mIsBatch = false;
}

Effect::~Effect()
{
   // Destroying what is usually the unique Effect object of its subclass,
   // which lasts until the end of the session.
   // Maybe there is a non-modal realtime dialog still open.
   if (mHostUIDialog)
      mHostUIDialog->Close();
}

// ComponentInterface implementation

PluginPath Effect::GetPath() const
{
   return BUILTIN_EFFECT_PREFIX + GetSymbol().Internal();
}

ComponentInterfaceSymbol Effect::GetSymbol() const
{
   return {};
}

VendorSymbol Effect::GetVendor() const
{
   return XO("Audacity");
}

wxString Effect::GetVersion() const
{
   return AUDACITY_VERSION_STRING;
}

TranslatableString Effect::GetDescription() const
{
   return {};
}

// EffectDefinitionInterface implementation

EffectType Effect::GetType() const
{
   return EffectTypeNone;
}

EffectFamilySymbol Effect::GetFamily() const
{
   // Unusually, the internal and visible strings differ for the built-in
   // effect family.
   return { wxT("Audacity"), XO("Built-in") };
}

bool Effect::IsInteractive() const
{
   return true;
}

bool Effect::IsDefault() const
{
   return true;
}

bool Effect::SupportsRealtime() const
{
   return false;
}

bool Effect::SupportsAutomation() const
{
   return true;
}

// EffectProcessor implementation

bool Effect::InitializeInstance(EffectSettings &settings)
{
   return true;
}

unsigned Effect::GetAudioInCount() const
{
   return 0;
}

unsigned Effect::GetAudioOutCount() const
{
   return 0;
}

int Effect::GetMidiInCount()
{
   return 0;
}

int Effect::GetMidiOutCount()
{
   return 0;
}

void Effect::SetSampleRate(double rate)
{
   mSampleRate = rate;
}

size_t Effect::SetBlockSize(size_t maxBlockSize)
{
   mBlockSize = maxBlockSize;
   return mBlockSize;
}

size_t Effect::GetBlockSize() const
{
   return mBlockSize;
}

sampleCount Effect::GetLatency()
{
   return 0;
}

size_t Effect::GetTailSize()
{
   return 0;
}

const EffectParameterMethods &Effect::Parameters() const
{
   static const CapturedParameters<Effect> empty;
   return empty;
}

bool Effect::ProcessInitialize(
   EffectSettings &settings, sampleCount totalLen, ChannelNames chanMap)
{
   return true;
}

bool Effect::ProcessFinalize()
{
   return true;
}

size_t Effect::ProcessBlock(EffectSettings &settings,
   const float *const *inBlock, float *const *outBlock, size_t blockLen)
{
   return 0;
}

bool Effect::RealtimeInitialize(EffectSettings &settings)
{
   return false;
}

bool Effect::RealtimeAddProcessor(
   EffectSettings &settings, unsigned numChannels, float sampleRate)
{
   return true;
}

bool Effect::RealtimeFinalize(EffectSettings &settings) noexcept
{
   return false;
}

bool Effect::RealtimeSuspend()
{
   return true;
}

bool Effect::RealtimeResume() noexcept
{
   return true;
}

bool Effect::RealtimeProcessStart(EffectSettings &settings)
{
   return true;
}

size_t Effect::RealtimeProcess(int group, EffectSettings &settings,
   const float *const *inbuf, float *const *outbuf, size_t numSamples)
{
   return 0;
}

bool Effect::RealtimeProcessEnd(EffectSettings &settings) noexcept
{
   return true;
}

int Effect::ShowClientInterface(
   wxWindow &parent, wxDialog &dialog, bool forceModal)
{
   // Remember the dialog with a weak pointer, but don't control its lifetime
   mUIDialog = &dialog;
   mUIDialog->Layout();
   mUIDialog->Fit();
   mUIDialog->SetMinSize(mUIDialog->GetSize());

   if ( VetoDialogHook::Call( mUIDialog ) )
      return 0;

   if( SupportsRealtime() && !forceModal )
   {
      mUIDialog->Show();
      // Return false to bypass effect processing
      return 0;
   }

   return mUIDialog->ShowModal();
}

int Effect::ShowHostInterface(wxWindow &parent,
   const EffectDialogFactory &factory, EffectSettingsAccess &access,
   bool forceModal)
{
   if (!IsInteractive())
      // Effect without UI just proceeds quietly to apply it destructively.
      return wxID_APPLY;

   if (mHostUIDialog)
   {
      // Realtime effect has shown its nonmodal dialog, now hides it, and does
      // nothing else.
      if ( mHostUIDialog->Close(true) )
         mHostUIDialog = nullptr;
      return 0;
   }

   // Create the dialog
   // Host, not client, is responsible for invoking the factory and managing
   // the lifetime of the dialog.
   // The usual factory lets the client (which is this, when self-hosting)
   // populate it.  That factory function is called indirectly through a
   // std::function to avoid source code dependency cycles.
   EffectUIClientInterface *const client = this;
   mHostUIDialog = factory(parent, *this, *client, access);
   if (!mHostUIDialog)
      return 0;

   // Let the client show the dialog and decide whether to keep it open
   auto result = client->ShowClientInterface(parent, *mHostUIDialog, forceModal);
   if (mHostUIDialog && !mHostUIDialog->IsShown())
      // Client didn't show it, or showed it modally and closed it
      // So destroy it.
      // (I think mHostUIDialog only needs to be a local variable in this
      // function -- that it is always null when the function begins -- but
      // that may change. PRL)
      mHostUIDialog->Destroy();

   return result;
}

bool Effect::VisitSettings(SettingsVisitor &visitor, EffectSettings &settings)
{
   Parameters().Visit(*this, visitor, settings);
   return true;
}

bool Effect::VisitSettings(
   ConstSettingsVisitor &visitor, const EffectSettings &settings) const
{
   Parameters().Visit(*this, visitor, settings);
   return true;
}

bool Effect::SaveSettings(
   const EffectSettings &settings, CommandParameters & parms) const
{
   Parameters().Get( *this, settings, parms );
   return true;
}

bool Effect::LoadSettings(
   const CommandParameters & parms, Settings &settings) const
{
   // The first argument, and with it the const_cast, will disappear when
   // all built-in effects are stateless.
   return Parameters().Set( *const_cast<Effect*>(this), parms, settings );
}

bool Effect::LoadUserPreset(
   const RegistryPath & name, EffectSettings &settings) const
{
   // Find one string in the registry and then reinterpret it
   // as complete settings
   wxString parms;
   if (!GetConfig(GetDefinition(), PluginSettings::Private,
      name, wxT("Parameters"), parms))
      return false;

   return LoadSettingsFromString(parms, settings);
}

bool Effect::SaveUserPreset(
   const RegistryPath & name, const EffectSettings &settings) const
{
   // Save all settings as a single string value in the registry
   wxString parms;
   if (!SaveSettingsAsString(settings, parms))
      return false;

   return SetConfig(GetDefinition(), PluginSettings::Private,
      name, wxT("Parameters"), parms);
}

RegistryPaths Effect::GetFactoryPresets() const
{
   return {};
}

bool Effect::LoadFactoryPreset(int id, EffectSettings &settings) const
{
   return true;
}

bool Effect::LoadFactoryDefaults(Settings &settings) const
{
   return LoadUserPreset(FactoryDefaultsGroup(), settings);
}

// EffectUIClientInterface implementation

std::unique_ptr<EffectUIValidator>
Effect::PopulateUI(ShuttleGui &S, EffectSettingsAccess &access)
{
   auto parent = S.GetParent();
   mUIParent = parent;

//   LoadUserPreset(CurrentSettingsGroup());

   // Let the effect subclass provide its own validator if it wants
   auto result = PopulateOrExchange(S, access);

   mUIParent->SetMinSize(mUIParent->GetSizer()->GetMinSize());

   if (!result) {
      // No custom validator object?  Then use the default
      result = std::make_unique<DefaultEffectUIValidator>(*this, access);
   }
   mUIParent->PushEventHandler(this);
   return result;
}

bool Effect::IsGraphicalUI()
{
   return false;
}

bool Effect::ValidateUI(EffectSettings &)
{
   return true;
}

bool Effect::CloseUI()
{
   if (mUIParent)
      mUIParent->RemoveEventHandler(this);

   mUIParent = NULL;
   mUIDialog = NULL;

   return true;
}

bool Effect::CanExportPresets()
{
   return true;
}

static const FileNames::FileTypes &PresetTypes()
{
   static const FileNames::FileTypes result {
      { XO("Presets"), { wxT("txt") }, true },
      FileNames::AllFiles
   };
   return result;
};

void Effect::ExportPresets(const EffectSettings &settings) const
{
   wxString params;
   SaveSettingsAsString(settings, params);
   auto commandId = GetSquashedName(GetSymbol().Internal());
   params =  commandId.GET() + ":" + params;

   auto path = SelectFile(FileNames::Operation::Presets,
                                     XO("Export Effect Parameters"),
                                     wxEmptyString,
                                     wxEmptyString,
                                     wxEmptyString,
                                     PresetTypes(),
                                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT | wxRESIZE_BORDER,
                                     nullptr);
   if (path.empty()) {
      return;
   }

   // Create/Open the file
   wxFFile f(path, wxT("wb"));
   if (!f.IsOpened())
   {
      AudacityMessageBox(
         XO("Could not open file: \"%s\"").Format( path ),
         XO("Error Saving Effect Presets"),
         wxICON_EXCLAMATION,
         NULL);
      return;
   }

   f.Write(params);
   if (f.Error())
   {
      AudacityMessageBox(
         XO("Error writing to file: \"%s\"").Format( path ),
         XO("Error Saving Effect Presets"),
         wxICON_EXCLAMATION,
         NULL);
   }

   f.Close();


   //SetWindowTitle();

}

void Effect::ImportPresets(EffectSettings &settings)
{
   wxString params;

   auto path = SelectFile(FileNames::Operation::Presets,
                                     XO("Import Effect Parameters"),
                                     wxEmptyString,
                                     wxEmptyString,
                                     wxEmptyString,
                                     PresetTypes(),
                                     wxFD_OPEN | wxRESIZE_BORDER,
                                     nullptr);
   if (path.empty()) {
      return;
   }

   wxFFile f(path);
   if (f.IsOpened()) {
      if (f.ReadAll(&params)) {
         wxString ident = params.BeforeFirst(':');
         params = params.AfterFirst(':');

         auto commandId = GetSquashedName(GetSymbol().Internal());

         if (ident != commandId) {
            // effect identifiers are a sensible length!
            // must also have some params.
            if ((params.Length() < 2 ) || (ident.Length() < 2) || (ident.Length() > 30)) 
            {
               Effect::MessageBox(
                  /* i18n-hint %s will be replaced by a file name */
                  XO("%s: is not a valid presets file.\n")
                  .Format(wxFileNameFromPath(path)));
            }
            else
            {
               Effect::MessageBox(
                  /* i18n-hint %s will be replaced by a file name */
                  XO("%s: is for a different Effect, Generator or Analyzer.\n")
                  .Format(wxFileNameFromPath(path)));
            }
            return;
         }
         LoadSettingsFromString(params, settings);
      }
   }

   //SetWindowTitle();

}

bool Effect::HasOptions()
{
   return false;
}

void Effect::ShowOptions()
{
}

// EffectUIHostInterface implementation

const EffectDefinitionInterface& Effect::GetDefinition() const
{
   return *this;
}

double Effect::GetDefaultDuration()
{
   return 30.0;
}

NumericFormatSymbol Effect::GetSelectionFormat()
{
   if( !IsBatchProcessing() && FindProject() )
      return ProjectSettings::Get( *FindProject() ).GetSelectionFormat();
   return NumericConverter::HoursMinsSecondsFormat();
}

wxString Effect::GetSavedStateGroup()
{
   return wxT("SavedState");
}

// Effect implementation

bool Effect::SaveSettingsAsString(
   const EffectSettings &settings, wxString & parms) const
{
   CommandParameters eap;
   ShuttleGetAutomation S;
   S.mpEap = &eap;
   if( VisitSettings( S, settings ) ){
      ;// got eap value using VisitSettings.
   }
   // Won't be needed in future
   else if (!SaveSettings(settings, eap))
   {
      return false;
   }

   return eap.GetParameters(parms);
}

bool Effect::LoadSettingsFromString(
   const wxString & parms, EffectSettings &settings) const
{
   // If the string starts with one of certain significant substrings,
   // then the rest of the string is reinterpreted as part of a registry key,
   // and a user or factory preset is then loaded.
   // (Where did these prefixes come from?  See EffectPresetsDialog; and
   // ultimately the uses of it by EffectManager::GetPreset, which is used by
   // the macro management dialog)
   wxString preset = parms;
   bool success = false;
   if (preset.StartsWith(kUserPresetIdent))
   {
      preset.Replace(kUserPresetIdent, wxEmptyString, false);
      success = LoadUserPreset(UserPresetsGroup(preset), settings);
   }
   else if (preset.StartsWith(kFactoryPresetIdent))
   {
      preset.Replace(kFactoryPresetIdent, wxEmptyString, false);
      auto presets = GetFactoryPresets();
      success = LoadFactoryPreset(
         make_iterator_range( presets ).index( preset ), settings );
   }
   else if (preset.StartsWith(kCurrentSettingsIdent))
   {
      preset.Replace(kCurrentSettingsIdent, wxEmptyString, false);
      success = LoadUserPreset(CurrentSettingsGroup(), settings);
   }
   else if (preset.StartsWith(kFactoryDefaultsIdent))
   {
      preset.Replace(kFactoryDefaultsIdent, wxEmptyString, false);
      success = LoadUserPreset(FactoryDefaultsGroup(), settings);
   }
   else
   {
      // If the string did not start with any of the significant substrings,
      // then use VisitSettings or LoadSettings to reinterpret it,
      // or use LoadSettings.
      // This interprets what was written by SaveSettings, above.
      CommandParameters eap(parms);
      ShuttleSetAutomation S;
      S.SetForValidating( &eap );
      // VisitSettings returns false if not defined for this effect.
      // To do: fix const_cast in use of VisitSettings
      if ( !const_cast<Effect*>(this)->VisitSettings(S, settings) )
         // the old method...
         success = LoadSettings(eap, settings);
      else if( !S.bOK )
         success = false;
      else{
         success = true;
         S.SetForWriting( &eap );
         const_cast<Effect*>(this)->VisitSettings(S, settings);
      }
   }

   if (!success)
   {
      Effect::MessageBox(
         XO("%s: Could not load settings below. Default settings will be used.\n\n%s")
            .Format( GetName(), preset ) );
      // We are using default settings and we still wish to continue.
      return true;
      //return false;
   }
   return true;
}

unsigned Effect::TestUIFlags(unsigned mask) {
   return mask & mUIFlags;
}

bool Effect::IsBatchProcessing() const
{
   return mIsBatch;
}

void Effect::SetBatchProcessing()
{
   mIsBatch = true;
   // Save effect's internal state in a special registry path
   // just for this purpose
   // If effect is not stateful, this step doesn't really matter, and the
   // settings object is a dummy
   auto dummySettings = MakeSettings();
   SaveUserPreset(GetSavedStateGroup(), dummySettings);
}

void Effect::UnsetBatchProcessing()
{
   mIsBatch = false;
   // Restore effect's internal state from registry
   // If effect is not stateful, this call doesn't really matter, and the
   // settings object is a dummy
   auto dummySettings = MakeSettings();
   LoadUserPreset(GetSavedStateGroup(), dummySettings);
}

// TODO:  Lift the possible user-prompting part out of this function, so that
// the recursive paths into this function via Effect::Delegate are simplified,
// and we don't have both EffectSettings and EffectSettingsAccessPtr
// If pAccess is not null, settings should have come from its Get()
bool Effect::DoEffect(EffectSettings &settings, double projectRate,
    TrackList *list,
    WaveTrackFactory *factory,
    NotifyingSelectedRegion &selectedRegion,
    unsigned flags,
    wxWindow *pParent,
    const EffectDialogFactory &dialogFactory,
    const EffectSettingsAccessPtr &pAccess)
{
   auto cleanup0 = valueRestorer(mUIFlags, flags);
   wxASSERT(selectedRegion.duration() >= 0.0);

   mOutputTracks.reset();

   mFactory = factory;
   mProjectRate = projectRate;
   mTracks = list;

   // This is for performance purposes only, no additional recovery implied
   auto &pProject = *const_cast<AudacityProject*>(FindProject()); // how to remove this const_cast?
   TransactionScope trans(pProject, "Effect");

   // Update track/group counts
   CountWaveTracks();

   bool isSelection = false;

   auto duration = 0.0;
   if (GetType() == EffectTypeGenerate)
      GetConfig(GetDefinition(), PluginSettings::Private,
         CurrentSettingsGroup(),
         EffectSettingsExtra::DurationKey(), duration, GetDefaultDuration());

   WaveTrack *newTrack{};
   bool success = false;
   auto oldDuration = duration;

   auto cleanup = finally( [&] {
      if (!success) {
         if (newTrack) {
            mTracks->Remove(newTrack);
         }
         // On failure, restore the old duration setting
         settings.extra.SetDuration(oldDuration);
      }
      else
         trans.Commit();

      End();
      ReplaceProcessedTracks( false );
   } );

   // We don't yet know the effect type for code in the Nyquist Prompt, so
   // assume it requires a track and handle errors when the effect runs.
   if ((GetType() == EffectTypeGenerate || GetPath() == NYQUIST_PROMPT_ID) && (mNumTracks == 0)) {
      auto track = mFactory->Create();
      track->SetName(mTracks->MakeUniqueTrackName(WaveTrack::GetDefaultAudioTrackNamePreference()));
      newTrack = mTracks->Add(track);
      newTrack->SetSelected(true);
   }

   mT0 = selectedRegion.t0();
   mT1 = selectedRegion.t1();
   if (mT1 > mT0)
   {
      // there is a selection: let's fit in there...
      // MJS: note that this is just for the TTC and is independent of the track rate
      // but we do need to make sure we have the right number of samples at the project rate
      double quantMT0 = QUANTIZED_TIME(mT0, mProjectRate);
      double quantMT1 = QUANTIZED_TIME(mT1, mProjectRate);
      duration = quantMT1 - quantMT0;
      isSelection = true;
      mT1 = mT0 + duration;
   }

   // This is happening inside EffectSettingsAccess::ModifySettings
   auto newFormat = isSelection
      ? NumericConverter::TimeAndSampleFormat()
      : NumericConverter::DefaultSelectionFormat();
   auto updater = [&](EffectSettings &settings) {
      settings.extra.SetDuration(duration);
      settings.extra.SetDurationFormat( newFormat );
   };
   // Update our copy of settings; update the EffectSettingsAccess too,
   // if we are going to show a dialog
   updater(settings);
   if (pAccess)
      pAccess->ModifySettings(updater);

#ifdef EXPERIMENTAL_SPECTRAL_EDITING
   mF0 = selectedRegion.f0();
   mF1 = selectedRegion.f1();
   wxArrayString Names;
   if( mF0 != SelectedRegion::UndefinedFrequency )
      Names.push_back(wxT("control-f0"));
   if( mF1 != SelectedRegion::UndefinedFrequency )
      Names.push_back(wxT("control-f1"));
   SetPresetParameters( &Names, NULL );

#endif
   CountWaveTracks();

   // Note: Init may read parameters from preferences
   if (!Init())
   {
      return false;
   }

   // Prompting will be bypassed when applying an effect that has already
   // been configured, e.g. repeating the last effect on a different selection.
   // Prompting may call Effect::Preview
   if ( pParent && dialogFactory && pAccess &&
      IsInteractive()) {
      if (!ShowHostInterface(
         *pParent, dialogFactory, *pAccess, IsBatchProcessing() ) )
         return false;
      else
         // Retrieve again after the dialog modified settings
         settings = pAccess->Get();
   }

   // If the dialog was shown, then it has been closed without errors or
   // cancellation, and any change of duration has been saved in the config file

   bool returnVal = true;
   bool skipFlag = CheckWhetherSkipEffect();
   if (skipFlag == false)
   {
      using namespace BasicUI;
      auto name = GetName();
      auto progress = MakeProgress(
         name,
         XO("Applying %s...").Format( name ),
         ProgressShowCancel
      );
      auto vr = valueRestorer( mProgress, progress.get() );

      returnVal = Process(settings);
   }

   if (returnVal && (mT1 >= mT0 ))
   {
      selectedRegion.setTimes(mT0, mT1);
   }

   success = returnVal;
   return returnVal;
}

bool Effect::Delegate(Effect &delegate, EffectSettings &settings,
   wxWindow &parent, const EffectDialogFactory &factory,
   const EffectSettingsAccessPtr &pSettings)
{
   NotifyingSelectedRegion region;
   region.setTimes( mT0, mT1 );

   return delegate.DoEffect(settings, mProjectRate, mTracks, mFactory,
      region, mUIFlags, &parent, factory, pSettings);
}

// All legacy effects should have this overridden
bool Effect::Init()
{
   return true;
}

bool Effect::InitPass1()
{
   return true;
}

bool Effect::InitPass2()
{
   return false;
}

bool Effect::Process(EffectSettings &settings)
{
   CopyInputTracks(true);
   bool bGoodResult = true;

   mPass = 1;
   if (InitPass1())
   {
      bGoodResult = ProcessPass(settings);
      mPass = 2;
      if (bGoodResult && InitPass2())
      {
         bGoodResult = ProcessPass(settings);
      }
   }

   ReplaceProcessedTracks(bGoodResult);

   return bGoodResult;
}

bool Effect::ProcessPass(EffectSettings &settings)
{
   const auto duration = settings.extra.GetDuration();
   bool bGoodResult = true;
   bool isGenerator = GetType() == EffectTypeGenerate;

   FloatBuffers inBuffer, outBuffer;
   ArrayOf<float *> inBufPos, outBufPos;

   ChannelName map[3];

   mBufferSize = 0;
   mBlockSize = 0;

   int count = 0;
   bool clear = false;

   // It's possible that the number of channels the effect expects changed based on
   // the parameters (the Audacity Reverb effect does when the stereo width is 0).
   const auto numAudioIn = GetAudioInCount();
   const auto numAudioOut = GetAudioOutCount();

   const bool multichannel = numAudioIn > 1;
   auto range = multichannel
      ? mOutputTracks->Leaders()
      : mOutputTracks->Any();
   range.VisitWhile( bGoodResult,
      [&](WaveTrack *left, const Track::Fallthrough &fallthrough) {
         if (!left->GetSelected())
            return fallthrough();

         sampleCount len = 0;
         sampleCount start = 0;

         mNumChannels = 0;
         WaveTrack *right{};

         // Iterate either over one track which could be any channel,
         // or if multichannel, then over all channels of left,
         // which is a leader.
         for (auto channel :
              TrackList::Channels(left).StartingWith(left)) {
            if (channel->GetChannel() == Track::LeftChannel)
               map[mNumChannels] = ChannelNameFrontLeft;
            else if (channel->GetChannel() == Track::RightChannel)
               map[mNumChannels] = ChannelNameFrontRight;
            else
               map[mNumChannels] = ChannelNameMono;

            ++ mNumChannels;
            map[mNumChannels] = ChannelNameEOL;

            if (! multichannel)
               break;

            if (mNumChannels == 2) {
               // TODO: more-than-two-channels
               right = channel;
               clear = false;
               // Ignore other channels
               break;
            }
         }

         if (!isGenerator)
         {
            GetBounds(*left, right, &start, &len);
            mSampleCnt = len;
         }
         else
            mSampleCnt = left->TimeToLongSamples(duration);

         // Let the client know the sample rate
         SetSampleRate(left->GetRate());

         // Get the block size the client wants to use
         auto max = left->GetMaxBlockSize() * 2;
         mBlockSize = SetBlockSize(max);

         // Calculate the buffer size to be at least the max rounded up to the clients
         // selected block size.
         const auto prevBufferSize = mBufferSize;
         mBufferSize = ((max + (mBlockSize - 1)) / mBlockSize) * mBlockSize;

         // If the buffer size has changed, then (re)allocate the buffers
         if (prevBufferSize != mBufferSize)
         {
            // Always create the number of input buffers the client expects even if we don't have
            // the same number of channels.
            inBufPos.reinit( numAudioIn );
            inBuffer.reinit( numAudioIn, mBufferSize );

            // We won't be using more than the first 2 buffers, so clear the rest (if any)
            for (size_t i = 2; i < numAudioIn; i++)
            {
               for (size_t j = 0; j < mBufferSize; j++)
               {
                  inBuffer[i][j] = 0.0;
               }
            }

            // Always create the number of output buffers the client expects even if we don't have
            // the same number of channels.
            outBufPos.reinit( numAudioOut );
            // Output buffers get an extra mBlockSize worth to give extra room if
            // the plugin adds latency
            outBuffer.reinit( numAudioOut, mBufferSize + mBlockSize );
         }

         // (Re)Set the input buffer positions
         for (size_t i = 0; i < numAudioIn; i++)
         {
            inBufPos[i] = inBuffer[i].get();
         }

         // (Re)Set the output buffer positions
         for (size_t i = 0; i < numAudioOut; i++)
         {
            outBufPos[i] = outBuffer[i].get();
         }

         // Clear unused input buffers
         if (!right && !clear && numAudioIn > 1)
         {
            for (size_t j = 0; j < mBufferSize; j++)
            {
               inBuffer[1][j] = 0.0;
            }
            clear = true;
         }

         // Go process the track(s)
         bGoodResult = ProcessTrack(settings,
            count, map, left, right, start, len,
            inBuffer, outBuffer, inBufPos, outBufPos);
         if (!bGoodResult)
            return;

         count++;
      },
      [&](Track *t) {
         if (SyncLock::IsSyncLockSelected(t))
            t->SyncLockAdjust(mT1, mT0 + duration);
      }
   );

   if (bGoodResult && GetType() == EffectTypeGenerate)
      mT1 = mT0 + duration;

   return bGoodResult;
}

bool Effect::ProcessTrack(EffectSettings &settings,
   int count,
   ChannelNames map,
   WaveTrack *left,
   WaveTrack *right,
   sampleCount start,
   sampleCount len,
   FloatBuffers &inBuffer,
   FloatBuffers &outBuffer,
   ArrayOf< float * > &inBufPos,
   ArrayOf< float *> &outBufPos)
{
   bool rc = true;

   // Give the plugin a chance to initialize
   if (!ProcessInitialize(settings, len, map))
   {
      return false;
   }

   { // Start scope for cleanup
   auto cleanup = finally( [&] {
      // Allow the plugin to cleanup
      if (!ProcessFinalize())
      {
         // In case of non-exceptional flow of control, set rc
         rc = false;
      }
   } );

   // For each input block of samples, we pass it to the effect along with a
   // variable output location.  This output location is simply a pointer into a
   // much larger buffer.  This reduces the number of calls required to add the
   // samples to the output track.
   //
   // Upon return from the effect, the output samples are "moved to the left" by
   // the number of samples in the current latency setting, effectively removing any
   // delay introduced by the effect.
   //
   // At the same time the total number of delayed samples are gathered and when
   // there is no further input data to process, the loop continues to call the
   // effect with an empty input buffer until the effect has had a chance to
   // return all of the remaining delayed samples.
   auto inPos = start;
   auto outPos = start;

   auto inputRemaining = len;
   decltype(GetLatency()) curDelay = 0, delayRemaining = 0;
   decltype(mBlockSize) curBlockSize = 0;

   decltype(mBufferSize) inputBufferCnt = 0;
   decltype(mBufferSize) outputBufferCnt = 0;
   bool cleared = false;

   auto chans = std::min<unsigned>(GetAudioOutCount(), mNumChannels);

   std::shared_ptr<WaveTrack> genLeft, genRight;

   decltype(len) genLength = 0;
   bool isGenerator = GetType() == EffectTypeGenerate;
   bool isProcessor = GetType() == EffectTypeProcess;
   double genDur = 0;
   if (isGenerator)
   {
      const auto duration = settings.extra.GetDuration();
      if (mIsPreview) {
         gPrefs->Read(wxT("/AudioIO/EffectsPreviewLen"), &genDur, 6.0);
         genDur = std::min(duration, CalcPreviewInputLength(settings, genDur));
      }
      else
         genDur = duration;

      genLength = sampleCount((left->GetRate() * genDur) + 0.5);  // round to nearest sample
      delayRemaining = genLength;
      cleared = true;

      // Create temporary tracks
      genLeft = left->EmptyCopy();

      if (right)
         genRight = right->EmptyCopy();
   }

   // Call the effect until we run out of input or delayed samples
   while (inputRemaining != 0 || delayRemaining != 0)
   {
      // Still working on the input samples
      if (inputRemaining != 0)
      {
         // Need to refill the input buffers
         if (inputBufferCnt == 0)
         {
            // Calculate the number of samples to get
            inputBufferCnt =
               limitSampleBufferSize( mBufferSize, inputRemaining );

            // Fill the input buffers
            left->GetFloats(inBuffer[0].get(), inPos, inputBufferCnt);
            if (right)
            {
               right->GetFloats(inBuffer[1].get(), inPos, inputBufferCnt);
            }

            // Reset the input buffer positions
            for (size_t i = 0; i < mNumChannels; i++)
            {
               inBufPos[i] = inBuffer[i].get();
            }
         }

         // Calculate the number of samples to process
         curBlockSize = mBlockSize;
         if (curBlockSize > inputRemaining)
         {
            // We've reached the last block...set current block size to what's left
            // inputRemaining is positive and bounded by a size_t
            curBlockSize = inputRemaining.as_size_t();
            inputRemaining = 0;

            // Clear the remainder of the buffers so that a full block can be passed
            // to the effect
            auto cnt = mBlockSize - curBlockSize;
            for (size_t i = 0; i < mNumChannels; i++)
            {
               for (decltype(cnt) j = 0 ; j < cnt; j++)
               {
                  inBufPos[i][j + curBlockSize] = 0.0;
               }
            }

            // Might be able to use up some of the delayed samples
            if (delayRemaining != 0)
            {
               // Don't use more than needed
               cnt = limitSampleBufferSize(cnt, delayRemaining);
               delayRemaining -= cnt;
               curBlockSize += cnt;
            }
         }
      }
      // We've exhausted the input samples and are now working on the delay
      else if (delayRemaining != 0)
      {
         // Calculate the number of samples to process
         curBlockSize = limitSampleBufferSize( mBlockSize, delayRemaining );
         delayRemaining -= curBlockSize;

         // From this point on, we only want to feed zeros to the plugin
         if (!cleared)
         {
            // Reset the input buffer positions
            for (size_t i = 0; i < mNumChannels; i++)
            {
               inBufPos[i] = inBuffer[i].get();

               // And clear
               for (size_t j = 0; j < mBlockSize; j++)
               {
                  inBuffer[i][j] = 0.0;
               }
            }
            cleared = true;
         }
      }

      // Finally call the plugin to process the block
      decltype(curBlockSize) processed;
      try
      {
         processed =
            ProcessBlock(settings, inBufPos.get(), outBufPos.get(), curBlockSize);
      }
      catch( const AudacityException & WXUNUSED(e) )
      {
         // PRL: Bug 437:
         // Pass this along to our application-level handler
         throw;
      }
      catch(...)
      {
         // PRL:
         // Exceptions for other reasons, maybe in third-party code...
         // Continue treating them as we used to, but I wonder if these
         // should now be treated the same way.
         return false;
      }
      wxASSERT(processed == curBlockSize);
      wxUnusedVar(processed);

      // Bump to next input buffer position
      if (inputRemaining != 0)
      {
         for (size_t i = 0; i < mNumChannels; i++)
         {
            inBufPos[i] += curBlockSize;
         }
         inputRemaining -= curBlockSize;
         inputBufferCnt -= curBlockSize;
      }

      // "ls" and "rs" serve as the input sample index for the left and
      // right channels when processing the input samples.  If we flip
      // over to processing delayed samples, they simply become counters
      // for the progress display.
      inPos += curBlockSize;

      // Get the current number of delayed samples and accumulate
      if (isProcessor)
      {
         {
            auto delay = GetLatency();
            curDelay += delay;
            delayRemaining += delay;
         }

         // If the plugin has delayed the output by more samples than our current
         // block size, then we leave the output pointers alone.  This effectively
         // removes those delayed samples from the output buffer.
         if (curDelay >= curBlockSize)
         {
            curDelay -= curBlockSize;
            curBlockSize = 0;
         }
         // We have some delayed samples, at the beginning of the output samples,
         // so overlay them by shifting the remaining output samples.
         else if (curDelay > 0)
         {
            // curDelay is bounded by curBlockSize:
            auto delay = curDelay.as_size_t();
            curBlockSize -= delay;
            for (size_t i = 0; i < chans; i++)
            {
               memmove(outBufPos[i], outBufPos[i] + delay, sizeof(float) * curBlockSize);
            }
            curDelay = 0;
         }
      }

      // Adjust the number of samples in the output buffers
      outputBufferCnt += curBlockSize;

      // Still have room in the output buffers
      if (outputBufferCnt < mBufferSize)
      {
         // Bump to next output buffer position
         for (size_t i = 0; i < chans; i++)
         {
            outBufPos[i] += curBlockSize;
         }
      }
      // Output buffers have filled
      else
      {
         if (isProcessor)
         {
            // Write them out
            left->Set((samplePtr) outBuffer[0].get(), floatSample, outPos, outputBufferCnt);
            if (right)
            {
               if (chans >= 2)
               {
                  right->Set((samplePtr) outBuffer[1].get(), floatSample, outPos, outputBufferCnt);
               }
               else
               {
                  right->Set((samplePtr) outBuffer[0].get(), floatSample, outPos, outputBufferCnt);
               }
            }
         }
         else if (isGenerator)
         {
            genLeft->Append((samplePtr) outBuffer[0].get(), floatSample, outputBufferCnt);
            if (genRight)
            {
               genRight->Append((samplePtr) outBuffer[1].get(), floatSample, outputBufferCnt);
            }
         }

         // Reset the output buffer positions
         for (size_t i = 0; i < chans; i++)
         {
            outBufPos[i] = outBuffer[i].get();
         }

         // Bump to the next track position
         outPos += outputBufferCnt;
         outputBufferCnt = 0;
      }

      if (mNumChannels > 1)
      {
         if (TrackGroupProgress(count,
               (inPos - start).as_double() /
               (isGenerator ? genLength : len).as_double()))
         {
            rc = false;
            break;
         }
      }
      else
      {
         if (TrackProgress(count,
               (inPos - start).as_double() /
               (isGenerator ? genLength : len).as_double()))
         {
            rc = false;
            break;
         }
      }
   }

   // Put any remaining output
   if (rc && outputBufferCnt)
   {
      if (isProcessor)
      {
         left->Set((samplePtr) outBuffer[0].get(), floatSample, outPos, outputBufferCnt);
         if (right)
         {
            if (chans >= 2)
            {
               right->Set((samplePtr) outBuffer[1].get(), floatSample, outPos, outputBufferCnt);
            }
            else
            {
               right->Set((samplePtr) outBuffer[0].get(), floatSample, outPos, outputBufferCnt);
            }
         }
      }
      else if (isGenerator)
      {
         genLeft->Append((samplePtr) outBuffer[0].get(), floatSample, outputBufferCnt);
         if (genRight)
         {
            genRight->Append((samplePtr) outBuffer[1].get(), floatSample, outputBufferCnt);
         }
      }
   }

   if (rc && isGenerator)
   {
      auto pProject = FindProject();

      // Transfer the data from the temporary tracks to the actual ones
      genLeft->Flush();
      // mT1 gives us the NEW selection. We want to replace up to GetSel1().
      auto &selectedRegion = ViewInfo::Get( *pProject ).selectedRegion;
      auto t1 = selectedRegion.t1();
      PasteTimeWarper warper{ t1, mT0 + genLeft->GetEndTime() };
      left->ClearAndPaste(mT0, t1, genLeft.get(), true, true,
         &warper);

      if (genRight)
      {
         genRight->Flush();
         right->ClearAndPaste(mT0, selectedRegion.t1(),
            genRight.get(), true, true, nullptr /* &warper */);
      }
   }

   } // End scope for cleanup
   return rc;
}

void Effect::End()
{
}

std::unique_ptr<EffectUIValidator>
Effect::PopulateOrExchange(ShuttleGui &, EffectSettingsAccess &)
{
   return nullptr;
}

bool Effect::TransferDataToWindow(const EffectSettings &)
{
   return true;
}

bool Effect::TransferDataFromWindow(EffectSettings &)
{
   return true;
}

bool Effect::EnableApply(bool enable)
{
   // May be called during initialization, so try to find the dialog
   wxWindow *dlg = mUIDialog;
   if (!dlg && mUIParent)
   {
      dlg = wxGetTopLevelParent(mUIParent);
   }

   if (dlg)
   {
      wxWindow *apply = dlg->FindWindow(wxID_APPLY);

      // Don't allow focus to get trapped
      if (!enable)
      {
         wxWindow *focus = dlg->FindFocus();
         if (focus == apply)
         {
            dlg->FindWindow(wxID_CLOSE)->SetFocus();
         }
      }

      apply->Enable(enable);
   }

   EnablePreview(enable);

   return enable;
}

bool Effect::EnablePreview(bool enable)
{
   // May be called during initialization, so try to find the dialog
   wxWindow *dlg = mUIDialog;
   if (!dlg && mUIParent)
   {
      dlg = wxGetTopLevelParent(mUIParent);
   }

   if (dlg)
   {
      wxWindow *play = dlg->FindWindow(kPlayID);
      if (play)
      {
         wxWindow *rewind = dlg->FindWindow(kRewindID);
         wxWindow *ffwd = dlg->FindWindow(kFFwdID);

         // Don't allow focus to get trapped
         if (!enable)
         {
            wxWindow *focus = dlg->FindFocus();
            if (focus && (focus == play || focus == rewind || focus == ffwd))
            {
               dlg->FindWindow(wxID_CLOSE)->SetFocus();
            }
         }

         play->Enable(enable);
         if (SupportsRealtime())
         {
            rewind->Enable(enable);
            ffwd->Enable(enable);
         }
      }
   }

   return enable;
}

void Effect::SetLinearEffectFlag(bool linearEffectFlag)
{
   mIsLinearEffect = linearEffectFlag;
}

void Effect::SetPreviewFullSelectionFlag(bool previewDurationFlag)
{
   mPreviewFullSelection = previewDurationFlag;
}


void Effect::IncludeNotSelectedPreviewTracks(bool includeNotSelected)
{
   mPreviewWithNotSelected = includeNotSelected;
}

bool Effect::TotalProgress(double frac, const TranslatableString &msg)
{
   auto updateResult = (mProgress ?
      mProgress->Poll(frac * 1000, 1000, msg) :
      ProgressResult::Success);
   return (updateResult != ProgressResult::Success);
}

bool Effect::TrackProgress(int whichTrack, double frac, const TranslatableString &msg)
{
   auto updateResult = (mProgress ?
      mProgress->Poll(whichTrack + frac, (double) mNumTracks, msg) :
      ProgressResult::Success);
   return (updateResult != ProgressResult::Success);
}

bool Effect::TrackGroupProgress(int whichGroup, double frac, const TranslatableString &msg)
{
   auto updateResult = (mProgress ?
      mProgress->Poll(whichGroup + frac, (double) mNumGroups, msg) :
      ProgressResult::Success);
   return (updateResult != ProgressResult::Success);
}

void Effect::GetBounds(
   const WaveTrack &track, const WaveTrack *pRight,
   sampleCount *start, sampleCount *len)
{
   auto t0 = std::max( mT0, track.GetStartTime() );
   auto t1 = std::min( mT1, track.GetEndTime() );

   if ( pRight ) {
      t0 = std::min( t0, std::max( mT0, pRight->GetStartTime() ) );
      t1 = std::max( t1, std::min( mT1, pRight->GetEndTime() ) );
   }

   if (t1 > t0) {
      *start = track.TimeToLongSamples(t0);
      auto end = track.TimeToLongSamples(t1);
      *len = end - *start;
   }
   else {
      *start = 0;
      *len = 0;
   }
}

//
// private methods
//
// Use this method to copy the input tracks to mOutputTracks, if
// doing the processing on them, and replacing the originals only on success (and not cancel).
// Copy the group tracks that have tracks selected
// If not all sync-locked selected, then only selected wave tracks.
void Effect::CopyInputTracks(bool allSyncLockSelected)
{
   // Reset map
   mIMap.clear();
   mOMap.clear();

   mOutputTracks = TrackList::Create(
      const_cast<AudacityProject*>( FindProject() ) // how to remove this const_cast?
  );

   auto trackRange = mTracks->Any() +
      [&] (const Track *pTrack) {
         return allSyncLockSelected
         ? SyncLock::IsSelectedOrSyncLockSelected(pTrack)
         : track_cast<const WaveTrack*>( pTrack ) && pTrack->GetSelected();
      };

   t2bHash added;

   for (auto aTrack : trackRange)
   {
      Track *o = mOutputTracks->Add(aTrack->Duplicate());
      mIMap.push_back(aTrack);
      mOMap.push_back(o);
   }
}

Track *Effect::AddToOutputTracks(const std::shared_ptr<Track> &t)
{
   mIMap.push_back(NULL);
   mOMap.push_back(t.get());
   return mOutputTracks->Add(t);
}

Effect::AddedAnalysisTrack::AddedAnalysisTrack(Effect *pEffect, const wxString &name)
   : mpEffect(pEffect)
{
   if(!name.empty())
      mpTrack = LabelTrack::Create(*pEffect->mTracks, name);
   else
      mpTrack = LabelTrack::Create(*pEffect->mTracks);
}

Effect::AddedAnalysisTrack::AddedAnalysisTrack(AddedAnalysisTrack &&that)
{
   mpEffect = that.mpEffect;
   mpTrack = that.mpTrack;
   that.Commit();
}

void Effect::AddedAnalysisTrack::Commit()
{
   mpEffect = nullptr;
}

Effect::AddedAnalysisTrack::~AddedAnalysisTrack()
{
   if (mpEffect) {
      // not committed -- DELETE the label track
      mpEffect->mTracks->Remove(mpTrack);
   }
}

auto Effect::AddAnalysisTrack(const wxString &name) -> std::shared_ptr<AddedAnalysisTrack>
{
   return std::shared_ptr<AddedAnalysisTrack>
      { safenew AddedAnalysisTrack{ this, name } };
}

Effect::ModifiedAnalysisTrack::ModifiedAnalysisTrack
   (Effect *pEffect, const LabelTrack *pOrigTrack, const wxString &name)
   : mpEffect(pEffect)
{
   // copy LabelTrack here, so it can be undone on cancel
   auto newTrack = pOrigTrack->Copy(pOrigTrack->GetStartTime(), pOrigTrack->GetEndTime());

   mpTrack = static_cast<LabelTrack*>(newTrack.get());

   // Why doesn't LabelTrack::Copy complete the job? :
   mpTrack->SetOffset(pOrigTrack->GetStartTime());
   if (!name.empty())
      mpTrack->SetName(name);

   // mpOrigTrack came from mTracks which we own but expose as const to subclasses
   // So it's okay that we cast it back to const
   mpOrigTrack =
      pEffect->mTracks->Replace(const_cast<LabelTrack*>(pOrigTrack),
         newTrack );
}

Effect::ModifiedAnalysisTrack::ModifiedAnalysisTrack(ModifiedAnalysisTrack &&that)
{
   mpEffect = that.mpEffect;
   mpTrack = that.mpTrack;
   mpOrigTrack = std::move(that.mpOrigTrack);
   that.Commit();
}

void Effect::ModifiedAnalysisTrack::Commit()
{
   mpEffect = nullptr;
}

Effect::ModifiedAnalysisTrack::~ModifiedAnalysisTrack()
{
   if (mpEffect) {
      // not committed -- DELETE the label track
      // mpOrigTrack came from mTracks which we own but expose as const to subclasses
      // So it's okay that we cast it back to const
      mpEffect->mTracks->Replace(mpTrack, mpOrigTrack);
   }
}

auto Effect::ModifyAnalysisTrack
   (const LabelTrack *pOrigTrack, const wxString &name) -> ModifiedAnalysisTrack
{
   return{ this, pOrigTrack, name };
}

// If bGoodResult, replace mTracks tracks with successfully processed mOutputTracks copies.
// Else clear and DELETE mOutputTracks copies.
void Effect::ReplaceProcessedTracks(const bool bGoodResult)
{
   if (!bGoodResult) {
      // Free resources, unless already freed.

      // Processing failed or was cancelled so throw away the processed tracks.
      if ( mOutputTracks )
         mOutputTracks->Clear();

      // Reset map
      mIMap.clear();
      mOMap.clear();

      //TODO:undo the non-gui ODTask transfer
      return;
   }

   // Assume resources need to be freed.
   wxASSERT(mOutputTracks); // Make sure we at least did the CopyInputTracks().

   auto iterOut = mOutputTracks->ListOfTracks::begin(),
      iterEnd = mOutputTracks->ListOfTracks::end();

   size_t cnt = mOMap.size();
   size_t i = 0;

   for (; iterOut != iterEnd; ++i) {
      ListOfTracks::value_type o = *iterOut;
      // If tracks were removed from mOutputTracks, then there will be
      // tracks in the map that must be removed from mTracks.
      while (i < cnt && mOMap[i] != o.get()) {
         const auto t = mIMap[i];
         if (t) {
            mTracks->Remove(t);
         }
         i++;
      }

      // This should never happen
      wxASSERT(i < cnt);

      // Remove the track from the output list...don't DELETE it
      iterOut = mOutputTracks->erase(iterOut);

      const auto  t = mIMap[i];
      if (t == NULL)
      {
         // This track is a NEW addition to output tracks; add it to mTracks
         mTracks->Add( o );
      }
      else
      {
         // Replace mTracks entry with the NEW track
         mTracks->Replace(t, o);
      }
   }

   // If tracks were removed from mOutputTracks, then there may be tracks
   // left at the end of the map that must be removed from mTracks.
   while (i < cnt) {
      const auto t = mIMap[i];
      if (t) {
         mTracks->Remove(t);
      }
      i++;
   }

   // Reset map
   mIMap.clear();
   mOMap.clear();

   // Make sure we processed everything
   wxASSERT(mOutputTracks->empty());

   // The output list is no longer needed
   mOutputTracks.reset();
   nEffectsDone++;
}

const AudacityProject *Effect::FindProject() const
{
   if (!inputTracks())
      return nullptr;
   return inputTracks()->GetOwner();
}

void Effect::CountWaveTracks()
{
   mNumTracks = mTracks->Selected< const WaveTrack >().size();
   mNumGroups = mTracks->SelectedLeaders< const WaveTrack >().size();
}

double Effect::CalcPreviewInputLength(
   const EffectSettings &, double previewLength)
{
   return previewLength;
}

void Effect::Preview(EffectSettingsAccess &access, bool dryOnly)
{
   if (mNumTracks == 0) { // nothing to preview
      return;
   }

   auto gAudioIO = AudioIO::Get();
   if (gAudioIO->IsBusy()) {
      return;
   }

   wxWindow *FocusDialog = wxWindow::FindFocus();

   double previewDuration;
   bool isNyquist = GetFamily() == NYQUISTEFFECTS_FAMILY;
   bool isGenerator = GetType() == EffectTypeGenerate;

   // Mix a few seconds of audio from all of the tracks
   double previewLen;
   gPrefs->Read(wxT("/AudioIO/EffectsPreviewLen"), &previewLen, 6.0);

   const double rate = mProjectRate;

   const auto &settings = access.Get();
   if (isNyquist && isGenerator)
      previewDuration = CalcPreviewInputLength(settings, previewLen);
   else
      previewDuration = std::min(settings.extra.GetDuration(),
         CalcPreviewInputLength(settings, previewLen));

   double t1 = mT0 + previewDuration;

   if ((t1 > mT1) && !isGenerator) {
      t1 = mT1;
   }

   if (t1 <= mT0)
      return;

   bool success = true;

   auto cleanup = finally( [&] {

      // Effect is already inited; we will call Process, End, and then Init
      // again, so the state is exactly the way it was before Preview
      // was called.
      if (!dryOnly) {
         End();
         GuardedCall( [&]{ Init(); } );
      }

      // In case any dialog control depends on mT1 or mDuration:
      if ( mUIDialog )
         mUIDialog->TransferDataToWindow();
   } );

   auto vr0 = valueRestorer( mT0 );
   auto vr1 = valueRestorer( mT1 );
   // Most effects should stop at t1.
   if (!mPreviewFullSelection)
      mT1 = t1;

   // In case any dialog control depends on mT1 or mDuration:
   if ( mUIDialog ) {
      mUIDialog->TransferDataToWindow();
   }

   // Save the original track list
   TrackList *saveTracks = mTracks;

   auto cleanup2 = finally( [&] {
      mTracks = saveTracks;
      if (FocusDialog) {
         FocusDialog->SetFocus();
      }

      // In case of failed effect, be sure to free memory.
      ReplaceProcessedTracks( false );
   } );

   // Build NEW tracklist from rendering tracks
   // Set the same owning project, so FindProject() can see it within Process()
   const auto pProject = saveTracks->GetOwner();
   auto uTracks = TrackList::Create( pProject );
   mTracks = uTracks.get();

   // Linear Effect preview optimised by pre-mixing to one track.
   // Generators need to generate per track.
   if (mIsLinearEffect && !isGenerator) {
      WaveTrack::Holder mixLeft, mixRight;
      MixAndRender(saveTracks, mFactory, rate, floatSample, mT0, t1, mixLeft, mixRight);
      if (!mixLeft)
         return;

      mixLeft->Offset(-mixLeft->GetStartTime());
      mixLeft->SetSelected(true);
      auto pLeft = mTracks->Add( mixLeft );
      Track *pRight{};
      if (mixRight) {
         mixRight->Offset(-mixRight->GetStartTime());
         mixRight->SetSelected(true);
         pRight = mTracks->Add( mixRight );
         mTracks->MakeMultiChannelTrack(*pLeft, 2, true);
      }
   }
   else {
      for (auto src : saveTracks->Any< const WaveTrack >()) {
         if (src->GetSelected() || mPreviewWithNotSelected) {
            auto dest = src->Copy(mT0, t1);
            dest->SetSelected(src->GetSelected());
            mTracks->Add( dest );
         }
      }
   }

   // NEW tracks start at time zero.
   // Adjust mT0 and mT1 to be the times to process, and to
   // play back in these tracks
   mT1 -= mT0;
   mT0 = 0.0;

   // Update track/group counts
   CountWaveTracks();

   // Apply effect
   if (!dryOnly) {
      using namespace BasicUI;
      auto progress = MakeProgress(
         GetName(),
         XO("Preparing preview"),
         ProgressShowStop
      ); // Have only "Stop" button.
      auto vr = valueRestorer( mProgress, progress.get() );

      auto vr2 = valueRestorer( mIsPreview, true );

      access.ModifySettings([&](EffectSettings &settings){
         success = Process(settings);
      });
   }

   if (success)
   {
      auto tracks = ProjectAudioManager::GetAllPlaybackTracks(*mTracks, true);

      // Some effects (Paulstretch) may need to generate more
      // than previewLen, so take the min.
      t1 = std::min(mT0 + previewLen, mT1);

      // Start audio playing
      auto options = DefaultPlayOptions(*pProject);
      int token = gAudioIO->StartStream(tracks, mT0, t1, t1, options);

      if (token) {
         auto previewing = ProgressResult::Success;
         // The progress dialog must be deleted before stopping the stream
         // to allow events to flow to the app during StopStream processing.
         // The progress dialog blocks these events.
         {
            ProgressDialog progress
            (GetName(), XO("Previewing"), pdlgHideCancelButton);

            while (gAudioIO->IsStreamActive(token) && previewing == ProgressResult::Success) {
               using namespace std::chrono;
               std::this_thread::sleep_for(100ms);
               previewing = progress.Update(gAudioIO->GetStreamTime() - mT0, t1 - mT0);
            }
         }

         gAudioIO->StopStream();

         while (gAudioIO->IsBusy()) {
            using namespace std::chrono;
            std::this_thread::sleep_for(100ms);
         }
      }
      else {
         using namespace BasicUI;
         ShowErrorDialog(
            wxWidgetsWindowPlacement{ FocusDialog }, XO("Error"),
            XO("Error opening sound device.\nTry changing the audio host, playback device and the project sample rate."),
            wxT("Error_opening_sound_device"),
            ErrorDialogOptions{ ErrorDialogType::ModalErrorReport } );
      }
   }
}

int Effect::MessageBox( const TranslatableString& message,
   long style, const TranslatableString &titleStr) const
{
   auto title = titleStr.empty()
      ? GetName()
      : XO("%s: %s").Format( GetName(), titleStr );
   return AudacityMessageBox( message, title, style, mUIParent );
}

