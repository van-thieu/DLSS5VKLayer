// The settings binder, tested without a window manager.
//
// Every control in the interface is one line of table, and the thing that line has to get right is
// invisible: the value has to reach the shared header, the right sequence number has to be bumped,
// and reloading has to not look like the user changing something. None of that is checked by the
// interface appearing on screen, which is the only test this project had before.
//
// Runs offscreen, so it needs no display:
//
//     QT_QPA_PLATFORM=offscreen ./build/binder_test
#include "shm_binder.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSlider>
#include <QWidget>

#include <cstdio>
#include <cstring>

static int failures = 0;
static void check(const char* what, bool ok) {
    std::printf("  %-42s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    ShmHeader hdr{};
    ShmInitDefaults(&hdr);

    QWidget page;
    auto* form = new QFormLayout(&page);
    ShmBinder binder(&hdr, &page);

    auto* enabled = binder.AddBool(form, "Enabled", &ShmHeader::enabled, "");
    auto* passes = qobject_cast<SliderEdit*>(binder.AddInt(form, "Passes", &ShmHeader::passes, 1, 30, ""));
    auto* detail = qobject_cast<SliderEdit*>(binder.AddFloat(form, "Detail", &ShmHeader::transferStrengthBits, 0, 4, 0.05, ""));
    auto* style = binder.AddChoice(form, "Style", &ShmHeader::style, { "a", "b", "c" }, "",
                                   ShmBinder::AtCreate);
    auto* scale = qobject_cast<SliderEdit*>(binder.AddPercent(form, "Scale", &ShmHeader::workingScaleBits, 25, 200, ""));

    const uint32_t tune0 = hdr.tuningSeq.load();

    enabled->setChecked(false);
    check("bool reaches the header", hdr.enabled.load() == 0);

    passes->setIntValue(4);
    check("int reaches the header", hdr.passes.load() == 4);

    detail->setDoubleValue(2.5);
    check("float reaches the header", BitsToFloat(hdr.transferStrengthBits.load()) == 2.5f);

    style->setCurrentIndex(2);
    check("choice reaches the header", hdr.style.load() == 2);

    scale->setIntValue(150);
    check("percent stores a fraction", BitsToFloat(hdr.workingScaleBits.load()) == 1.5f);

    check("a create-time setting bumps tuningSeq", hdr.tuningSeq.load() > tune0);

    const uint32_t tune1 = hdr.tuningSeq.load();
    detail->setDoubleValue(1.0);
    check("a live setting does not bump tuningSeq", hdr.tuningSeq.load() == tune1);

    // The other direction: change the header underneath and reload.
    hdr.enabled.store(1);
    hdr.passes.store(7);
    hdr.transferStrengthBits.store(FloatToBits(3.25f));
    hdr.workingScaleBits.store(FloatToBits(0.75f));
    const uint32_t ctrl = hdr.controlSeq.load();
    binder.Reload();
    check("reload pulls bool", enabled->isChecked());
    check("reload pulls int", passes->intValue() == 7);
    check("reload pulls float", detail->doubleValue() == 3.25);
    check("reload pulls percent", scale->intValue() == 75);
    check("reload does not write back", hdr.controlSeq.load() == ctrl);

    // The two edit-field bugs: the once-a-second reload used to overwrite a number the user was
    // halfway through typing, and committing a typed value used to emit a second, quantised value
    // from the slider handler and replace the text with that approximation.
    auto* edit = detail->findChild<QLineEdit*>();
    page.show();  // hasFocus() is only meaningful once the widget is visible
    page.activateWindow();
    QApplication::setActiveWindow(&page);
    edit->setFocus();
    QApplication::processEvents();
    edit->setText("2.75");
    hdr.transferStrengthBits.store(FloatToBits(1.0f));
    binder.Reload();
    check("reload leaves an active edit alone", edit->text() == "2.75");
    edit->clearFocus();

    edit->setText("2.37");
    QMetaObject::invokeMethod(edit, "editingFinished");
    check("typed value is not rounded by the slider",
          BitsToFloat(hdr.transferStrengthBits.load()) == 2.37f && edit->text() == "2.37");

    std::printf("%s\n", failures ? "FAILURES" : "all binder paths ok");
    return failures ? 1 : 0;
}
