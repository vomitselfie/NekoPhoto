// The text editor's QTextDocument and a text layer's runs: every field and every UTF-16 position survives the round
// trip, newlines included, and an edit in the document comes back as runs.
#include "check.h"
#include "RichText.h"
#include <QGuiApplication>
#include <QTextCursor>
#include <QTextDocument>

using namespace app;
using namespace compositor;

namespace {

LayerText styled() {
    LayerText t;
    t.text = "Big \xF0\x9F\x98\x80 small\nsecond line\n\nend";   // an emoji (two units), newlines, an empty line
    t.alignment = 1;
    t.lineSpacing = 1.3;
    TextRun a; a.length = 4; a.fontFamily = "DejaVu Serif"; a.fontSize = 72.25; a.bold = true; a.weight = 800; a.red = 0.1; a.green = 0.2; a.blue = 0.3;
    TextRun b = a; b.length = 2; b.fontSize = 12; b.baselineShift = 3.5; b.caps = TextRun::Caps::Small; b.bold = false; b.weight = 0;
    TextRun c = b; c.length = 7; c.fontFamily = ""; c.italic = true; c.underline = true; c.letterSpacing = -1.25; c.leading = 40;   // " small\n"
    TextRun d = c; d.length = 5; d.strikethrough = true; d.caps = TextRun::Caps::All; d.red = 1;
    TextRun e = a; e.length = utf16Length(t.text) - 18; e.fontSize = 20;   // the rest, the empty line's breaks included
    t.runs = {a, b, c, d, e};
    settleTextRuns(t);
    return t;
}

} // namespace

TEST_CASE(runs_round_trip_through_a_text_document) {
    const LayerText t = styled();
    REQUIRE(t.runs.size() == 5);
    QTextDocument doc;
    fillTextDocument(doc, t, 0.5);
    CHECK_EQ(doc.characterCount() - 1, utf16Length(t.text));
    LayerText back = layerTextFromDocument(doc, t, baseTextRun(t));
    CHECK(back.text == t.text);
    CHECK(back.runs == t.runs);
    CHECK(back == t);
}

TEST_CASE(a_run_ending_at_a_newline_keeps_it) {
    // The break is its own character: a run that ends just after it, and one that starts with it.
    LayerText t;
    t.text = "ab\ncd";
    TextRun a; a.length = 3; a.fontSize = 10;
    TextRun b; b.length = 2; b.fontSize = 30;
    t.runs = {a, b};
    settleTextRuns(t);
    QTextDocument doc;
    fillTextDocument(doc, t, 1);
    CHECK(layerTextFromDocument(doc, t, baseTextRun(t)).runs == t.runs);
    t.runs[0].length = 2; t.runs[1].length = 3;
    fillTextDocument(doc, t, 1);
    CHECK(layerTextFromDocument(doc, t, baseTextRun(t)).runs == t.runs);
}

TEST_CASE(typing_takes_the_style_of_the_run_it_is_in) {
    const LayerText t = styled();
    QTextDocument doc;
    fillTextDocument(doc, t, 1);
    QTextCursor cursor(&doc);
    cursor.setPosition(2);   // inside "Big"
    cursor.insertText("ggg");
    LayerText back = layerTextFromDocument(doc, t, baseTextRun(t));
    CHECK(back.runs[0].length == 7);
    CHECK(back.runs[1] == t.runs[1]);
    // A range restyled through a merged format, as the Character section does.
    cursor.setPosition(0);
    cursor.setPosition(3, QTextCursor::KeepAnchor);
    TextRun red = runFromFormat(cursor.charFormat(), baseTextRun(t));
    red.red = 1; red.green = 0; red.blue = 0;
    cursor.mergeCharFormat(charFormatFor(red, 1));
    back = layerTextFromDocument(doc, t, baseTextRun(t));
    REQUIRE(back.runs.size() == 6);
    CHECK(back.runs[0].length == 3 && back.runs[0].red == 1 && back.runs[1].length == 4 && back.runs[1].red == t.runs[0].red);
    // documentRuns over a stretch.
    auto part = documentRuns(doc, baseTextRun(t), 1, 5);
    REQUIRE(part.size() == 2);
    CHECK(part[0].length == 2 && part[1].length == 2);
}

TEST_CASE(emptied_text_keeps_its_style) {
    LayerText t = styled();
    QTextDocument doc;
    fillTextDocument(doc, t, 1);
    QTextCursor cursor(&doc);
    cursor.select(QTextCursor::Document);
    cursor.removeSelectedText();
    LayerText back = layerTextFromDocument(doc, t, baseTextRun(t));
    CHECK(back.text.empty() && back.runs.empty());
    CHECK(back.fontSize == 72.25 && back.fontFamily == "DejaVu Serif");
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    return check::run(argc, argv);
}
