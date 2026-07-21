#include <gtest/gtest.h>

#include "markdown/markdown_formatter.hpp"
#include "markdown/mermaid_renderer.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>

namespace {

using acecode::markdown::MermaidArt;
using acecode::markdown::render_mermaid_terminal;

std::string plain(const MermaidArt& art) {
    std::string out;
    for (const auto& line : art.lines) {
        if (!out.empty()) out += '\n';
        out += line.plain_text();
    }
    return out;
}

void expect_bounded(const MermaidArt& art, int width) {
    ASSERT_FALSE(art.lines.empty());
    for (const auto& line : art.lines) {
        EXPECT_LE(line.display_width(), width) << line.plain_text();
    }
}

TEST(MermaidRenderer, FlowchartRendersNodesEdgesDirectionAndSubgraph) {
    const auto art = render_mermaid_terminal(R"(flowchart LR
subgraph api[API Layer]
  A[Start] -->|request| B(Handle)
end
B ==> C{Done?}
)", 100);

    ASSERT_TRUE(art.has_value());
    EXPECT_FALSE(art->fallback);
    const auto output = plain(*art);
    EXPECT_NE(output.find("API Layer"), std::string::npos);
    EXPECT_NE(output.find("Start"), std::string::npos);
    EXPECT_NE(output.find("Handle"), std::string::npos);
    EXPECT_NE(output.find("Done?"), std::string::npos);
    EXPECT_NE(output.find("request"), std::string::npos);
    EXPECT_NE(output.find("━"), std::string::npos);
}

TEST(MermaidRenderer, StateDiagramRendersMarkersChoiceAndTransitions) {
    const auto art = render_mermaid_terminal(R"(stateDiagram-v2
[*] --> Idle
state Choice <<choice>>
Idle --> Choice: decide
Choice --> Running
Running --> [*]
)", 100);

    ASSERT_TRUE(art.has_value());
    EXPECT_FALSE(art->fallback);
    const auto output = plain(*art);
    EXPECT_NE(output.find("●"), std::string::npos);
    EXPECT_NE(output.find("◉"), std::string::npos);
    EXPECT_NE(output.find("Choice"), std::string::npos);
    EXPECT_NE(output.find("decide"), std::string::npos);
    EXPECT_NE(output.find("◇"), std::string::npos);
}

TEST(MermaidRenderer, ClassDiagramRendersCompartmentsAndRelationships) {
    const auto art = render_mermaid_terminal(R"(classDiagram
class Animal {
  +String name
  +speak()
}
class Dog
Animal <|-- Dog : inherits
)", 100);

    ASSERT_TRUE(art.has_value());
    EXPECT_FALSE(art->fallback);
    const auto output = plain(*art);
    EXPECT_NE(output.find("Animal"), std::string::npos);
    EXPECT_NE(output.find("+String name"), std::string::npos);
    EXPECT_NE(output.find("+speak()"), std::string::npos);
    EXPECT_NE(output.find("inherits"), std::string::npos);
    EXPECT_TRUE(output.find("△") != std::string::npos ||
                output.find("▽") != std::string::npos ||
                output.find("◁") != std::string::npos ||
                output.find("▷") != std::string::npos);
}

TEST(MermaidRenderer, ErDiagramRendersAttributesCardinalityAndDottedStyle) {
    const auto art = render_mermaid_terminal(R"(erDiagram
CUSTOMER ||..o{ ORDER : places
ORDER {
  int orderNumber PK
  string status
}
)", 100);

    ASSERT_TRUE(art.has_value());
    EXPECT_FALSE(art->fallback);
    const auto output = plain(*art);
    EXPECT_NE(output.find("CUSTOMER"), std::string::npos);
    EXPECT_NE(output.find("ORDER"), std::string::npos);
    EXPECT_NE(output.find("orderNumber PK"), std::string::npos);
    EXPECT_NE(output.find("1 places 0..*"), std::string::npos);
    EXPECT_TRUE(output.find("┄") != std::string::npos ||
                output.find("┆") != std::string::npos);
}

TEST(MermaidRenderer, SequenceRendersParticipantsMessagesNotesBlocksAndAutonumber) {
    const auto art = render_mermaid_terminal(R"(sequenceDiagram
autonumber
participant A as Alice
participant B as Bob
A->>B: Hello
Note over A,B: Shared note
loop Retry
B-->>A: Result
end
)", 100);

    ASSERT_TRUE(art.has_value());
    EXPECT_FALSE(art->fallback);
    const auto output = plain(*art);
    EXPECT_NE(output.find("Alice"), std::string::npos);
    EXPECT_NE(output.find("Bob"), std::string::npos);
    EXPECT_NE(output.find("1. Hello"), std::string::npos);
    EXPECT_NE(output.find("Shared note"), std::string::npos);
    EXPECT_NE(output.find("loop Retry"), std::string::npos);
    EXPECT_NE(output.find("2. Result"), std::string::npos);
}

TEST(MermaidRenderer, WideGlyphLabelsStayAlignedAndBounded) {
    const auto art = render_mermaid_terminal(
        "flowchart TD\nA[中文节点] --> B[完成状态]\n", 80);

    ASSERT_TRUE(art.has_value());
    EXPECT_FALSE(art->fallback);
    const auto output = plain(*art);
    EXPECT_NE(output.find("中文节点"), std::string::npos) << output;
    expect_bounded(*art, 80);
}

TEST(MermaidRenderer, StraightChainDoesNotCreateFalseCrossing) {
    const auto art = render_mermaid_terminal(
        "flowchart TD\nA[Start] --> B[Done]\n", 80);

    ASSERT_TRUE(art.has_value());
    ASSERT_FALSE(art->fallback);
    const auto output = plain(*art);
    EXPECT_EQ(output.find("┼"), std::string::npos) << output;
    EXPECT_NE(output.find("│"), std::string::npos);
}

TEST(MermaidRenderer, NarrowDiagramUsesBoundedTooWideFallback) {
    const auto art = render_mermaid_terminal(
        "flowchart LR\n"
        "A[This is a long starting node] --> "
        "B[This is another long destination node]\n",
        24);

    ASSERT_TRUE(art.has_value());
    EXPECT_TRUE(art->fallback);
    EXPECT_TRUE(art->too_wide);
    EXPECT_NE(plain(*art).find("too wide"), std::string::npos);
    expect_bounded(*art, 24);
}

TEST(MermaidRenderer, UnsupportedAndInvalidDiagramsPreserveSource) {
    const auto unsupported = render_mermaid_terminal(
        "gantt\n  title Release plan\n", 60);
    ASSERT_TRUE(unsupported.has_value());
    EXPECT_TRUE(unsupported->fallback);
    EXPECT_FALSE(unsupported->too_wide);
    EXPECT_NE(plain(*unsupported).find("mermaid: gantt"), std::string::npos);
    EXPECT_NE(plain(*unsupported).find("Release plan"), std::string::npos);

    const auto invalid = render_mermaid_terminal(
        "classDiagram\nthis statement cannot be represented\n", 60);
    ASSERT_TRUE(invalid.has_value());
    EXPECT_TRUE(invalid->fallback);
    EXPECT_NE(plain(*invalid).find("this statement"), std::string::npos);
}

TEST(MermaidRenderer, ComplexityCapFallsBackWithoutDroppingSource) {
    std::ostringstream source;
    source << "flowchart TD\n";
    for (int i = 0; i < 130; ++i) {
        source << "N" << i << " --> N" << (i + 1) << "\n";
    }
    const auto art = render_mermaid_terminal(source.str(), 80);

    ASSERT_TRUE(art.has_value());
    EXPECT_TRUE(art->fallback);
    EXPECT_NE(plain(*art).find("N129"), std::string::npos);
    expect_bounded(*art, 80);
}

TEST(MermaidRenderer, BlankSourceReturnsNoDiagram) {
    EXPECT_FALSE(render_mermaid_terminal(" \r\n\t", 80).has_value());
}

TEST(MermaidRenderer, MarkdownFenceUsesDiagramWhileOtherCodeKeepsLanguageLabel) {
    acecode::markdown::FormatOptions options;
    options.terminal_width = 80;
    const auto diagram = acecode::markdown::format_markdown(
        "```MerMaid theme=dark\nflowchart TD\nA[Start] --> B[Done]\n```\n",
        options);
    ftxui::Screen diagram_screen(80, 20);
    ftxui::Render(diagram_screen, diagram);
    const auto diagram_output = diagram_screen.ToString();
    EXPECT_NE(diagram_output.find("Start"), std::string::npos);
    EXPECT_NE(diagram_output.find("Done"), std::string::npos);
    EXPECT_EQ(diagram_output.find("flowchart TD"), std::string::npos);

    const auto code = acecode::markdown::format_markdown(
        "```cpp\nint answer = 42;\n```\n", options);
    ftxui::Screen code_screen(80, 6);
    ftxui::Render(code_screen, code);
    const auto code_output = code_screen.ToString();
    EXPECT_NE(code_output.find("cpp"), std::string::npos);
    EXPECT_NE(code_output.find("answer"), std::string::npos);

    acecode::markdown::FormatOptions narrow_options;
    narrow_options.terminal_width = 28;
    const auto narrow = acecode::markdown::format_markdown(
        "```mermaid\nflowchart LR\n"
        "A[A very long starting node] --> B[A very long ending node]\n"
        "```\n",
        narrow_options);
    ftxui::Screen narrow_screen(28, 24);
    ftxui::Render(narrow_screen, narrow);
    const auto narrow_output = narrow_screen.ToString();
    EXPECT_NE(narrow_output.find("mermaid:"), std::string::npos);
    EXPECT_NE(narrow_output.find("too wide"), std::string::npos);
}

} // namespace
