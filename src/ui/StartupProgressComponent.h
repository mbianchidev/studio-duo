#pragma once

#include "startup/StartupCoordinator.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace studio
{
class StartupProgressComponent final : public juce::Component
{
public:
    StartupProgressComponent();
    void setSnapshot(const StartupCoordinator& startup, bool workspaceAvailable);
    void setNotice(const juce::String& message);
    [[nodiscard]] bool hasPainted() const noexcept;

    std::function<void()> onFirstPaint;
    std::function<void()> onRetry;
    std::function<void()> onOpenIo;
    std::function<void()> onWithoutAudio;
    std::function<void()> onSafeStart;
    std::function<void()> onResume;
    std::function<void()> onCancelScan;
    std::function<void()> onExport;
    std::function<void()> onClose;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    class Progress final : public juce::Component
    {
    public:
        void setValue(double value);
        void paint(juce::Graphics&) override;
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
        double value = 0.0;
    };

    juce::Label title;
    juce::Label build;
    juce::Label current;
    juce::Label notice;
    Progress progress;
    juce::TextButton historyButton { "Show step history" };
    juce::TextEditor history;
    juce::TextButton retry { "Retry" };
    juce::TextButton openIo { "Open I/O" };
    juce::TextButton withoutAudio { "Start without audio" };
    juce::TextButton safeStart { "Safe start" };
    juce::TextButton resume { "Continue normally" };
    juce::TextButton cancelScan { "Cancel background scan" };
    juce::TextButton exportReport { "Export diagnostics" };
    juce::TextButton close { "Back to workspace" };
    bool painted = false;
    bool canClose = false;
    juce::String lastAnnouncement;
};
}
