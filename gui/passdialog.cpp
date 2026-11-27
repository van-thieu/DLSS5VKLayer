#include "passdialog.h"
#include "shm_binder.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>

PassDialog::PassDialog(ShmHeader* header, QWidget* parent) : QDialog(parent), hdr(header) {
    setWindowTitle(tr("Per-pass model settings"));
    resize(460, 460);

    auto* root = new QVBoxLayout(this);
    auto* note = new QLabel(
        tr("Turn on \"Use per-pass settings\" for a pass to take it over from the main controls. "
        "Inside it, a pass follows the global settings for anything it does not tick. These are read "
        "when that pass's feature is built, so a change takes a moment to appear."),
        this);
    note->setWordWrap(true);
    root->addWidget(note);

    averageToggle = new QCheckBox(
        tr("Average pass strengths (share intensity across active passes)"), this);
    averageToggle->setToolTip(tr("On: each active pass contributes intensity / number of active "
                                 "passes, so the last pass cannot set the whole picture's strength. "
                                 "Off: each pass uses its own intensity directly."));
    if (hdr) averageToggle->setChecked(hdr->passAverage.load() != 0);
    connect(averageToggle, &QCheckBox::toggled, this, [this](bool on) {
        if (!hdr) return;
        hdr->passAverage.store(on ? 1u : 0u);
        hdr->controlSeq.fetch_add(1);
    });
    root->addWidget(averageToggle);

    tabs = new QTabWidget(this);
    // Only the passes that can actually run are offered; the ceiling is a real limit, not a hint.
    const uint32_t shown = hdr ? ShmPassCeiling(hdr) : kMaxPasses;
    for (uint32_t i = 0; i < shown; ++i) {
        auto* page = new QWidget(tabs);
        buildPage(i, page);
        applyEnabled(i, enabled[i] && enabled[i]->isChecked());
        tabs->addTab(page, QString(tr("Pass %1")).arg(i + 1));
    }
    root->addWidget(tabs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    root->addWidget(buttons);
}

void PassDialog::buildPage(uint32_t pass, QWidget* page) {
    auto* form = new QFormLayout(page);
    if (!hdr) return;

    const uint32_t mask = hdr->pass[pass].overrideMask.load();
    PassControl& pc = hdr->pass[pass];
    pageWidget[pass] = page;

    // One toggle per pass: on means this pass uses its own settings and the main
    // model controls are read-only; off means it follows the main controls again.
    enabled[pass] = new QCheckBox(tr("Use per-pass settings for this pass"), page);
    enabled[pass]->setChecked((mask & kPassEnabled) != 0);
    enabled[pass]->setToolTip(tr("While any pass has this on, the main model controls are disabled "
                                 "so the two sets of settings cannot disagree."));
    auto* header = new QHBoxLayout;
    header->addWidget(enabled[pass]);
    form->addRow(header);
    connect(enabled[pass], &QCheckBox::toggled, this, [this, pass](bool on) {
        applyEnabled(pass, on);
        writePass(pass);
    });

    const auto addRow = [&](const QString& label, uint32_t bit, QWidget* value,
                            std::function<uint32_t()> read, std::function<void(uint32_t)> write) {
        auto* on = new QCheckBox(page);
        on->setChecked((mask & bit) != 0);
        value->setEnabled(on->isChecked() && enabled[pass]->isChecked());

        auto* row = new QHBoxLayout;
        row->addWidget(on);
        row->addWidget(value, 1);
        form->addRow(label, row);

        Row r{ on, value, bit, std::move(read), std::move(write) };
        rows[pass].push_back(r);

        connect(on, &QCheckBox::toggled, this, [this, pass, value](bool checked) {
            value->setEnabled(checked && enabled[pass]->isChecked());
            writePass(pass);
        });
    };

    const auto makeFloat = [&](double lo, double hi, double step, uint32_t bits) {
        auto* w = new SliderEdit(page, lo, hi, step, double(BitsToFloat(bits)));
        connect(w, &SliderEdit::doubleChanged, this,
                [this, pass](double) { writePass(pass); });
        return w;
    };

    auto* intensity = makeFloat(0.0, 1.0, 0.05, pc.intensityBits.load());
    addRow(tr("Intensity"), kOverrideIntensity, intensity,
           [intensity] { return FloatToBits(float(intensity->doubleValue())); }, {});

    auto* structure = makeFloat(0.0, 1.0, 0.05, pc.localStructureBits.load());
    addRow(tr("Local structure"), kOverrideLocalStructure, structure,
           [structure] { return FloatToBits(float(structure->doubleValue())); }, {});

    auto* tone = makeFloat(0.0, 1.0, 0.05, pc.localToneBits.load());
    addRow(tr("Local tone"), kOverrideLocalTone, tone,
           [tone] { return FloatToBits(float(tone->doubleValue())); }, {});

    auto* skin = makeFloat(-1.0, 1.0, 0.05, pc.skinStructureBits.load());
    addRow(tr("Skin structure"), kOverrideSkinStructure, skin,
           [skin] { return FloatToBits(float(skin->doubleValue())); }, {});

    auto* sharp = makeFloat(0.0, 1.0, 0.05, pc.sharpnessBits.load());
    addRow(tr("Sharpness"), kOverrideSharpness, sharp,
           [sharp] { return FloatToBits(float(sharp->doubleValue())); }, {});

    auto* style = new QComboBox(page);
    style->addItems({ tr("Default"), tr("Natural"), tr("Cinematic") });
    style->setCurrentIndex(int(pc.style.load()));
    connect(style, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, pass](int) { writePass(pass); });
    addRow(tr("Style"), kOverrideStyle, style, [style] { return uint32_t(style->currentIndex()); }, {});

    auto* preset = new SliderEdit(page, 0, 15, 1, int(pc.preset.load()));
    connect(preset, &SliderEdit::intChanged, this,
            [this, pass](int) { writePass(pass); });
    addRow(tr("Preset"), kOverridePreset, preset, [preset] { return uint32_t(preset->intValue()); }, {});

    // A combo rather than a second checkbox: the row already has the override
    // tick, and two checkboxes side by side made it unclear which one did what.
    auto* automask = new QComboBox(page);
    automask->addItems({ tr("off"), tr("on") });
    automask->setCurrentIndex(pc.autoMask.load() ? 1 : 0);
    connect(automask, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, pass](int) { writePass(pass); });
    addRow(tr("Auto skin mask"), kOverrideAutoMask, automask,
           [automask] { return uint32_t(automask->currentIndex()); }, {});
}

void PassDialog::writePass(uint32_t pass) {
    if (!hdr || pass >= kMaxPasses) return;
    PassControl& pc = hdr->pass[pass];

    uint32_t mask = enabled[pass] && enabled[pass]->isChecked() ? kPassEnabled : 0u;
    for (const Row& r : rows[pass]) {
        if (!r.on->isChecked()) continue;
        mask |= r.bit;
        const uint32_t v = r.read();
        switch (r.bit) {
            case kOverrideIntensity: pc.intensityBits.store(v); break;
            case kOverrideLocalStructure: pc.localStructureBits.store(v); break;
            case kOverrideLocalTone: pc.localToneBits.store(v); break;
            case kOverrideSkinStructure: pc.skinStructureBits.store(v); break;
            case kOverrideSharpness: pc.sharpnessBits.store(v); break;
            case kOverrideStyle: pc.style.store(v); break;
            case kOverridePreset: pc.preset.store(v); break;
            case kOverrideAutoMask: pc.autoMask.store(v); break;
            default: break;
        }
    }
    pc.overrideMask.store(mask);
    hdr->controlSeq.fetch_add(1);
    // Every one of these is latched when the pass's feature is built, so the helper has to know to
    // rebuild rather than carry on with a feature made from the old values.
    hdr->tuningSeq.fetch_add(1);
}

void PassDialog::applyEnabled(uint32_t pass, bool on) {
    if (pass >= kMaxPasses) return;
    for (const Row& r : rows[pass]) {
        if (r.on) r.on->setEnabled(on);
        if (r.value) r.value->setEnabled(on && r.on && r.on->isChecked());
    }
}
