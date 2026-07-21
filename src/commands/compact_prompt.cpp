#include "compact_prompt.hpp"

#include <string>

namespace acecode {

namespace {

const std::string kCompactPrompt =
    "You are performing a CONTEXT CHECKPOINT COMPACTION. Create a handoff summary for another LLM that will resume the task.\n\n"
    "Include:\n"
    "- Current progress and key decisions made\n"
    "- Important context, constraints, or user preferences\n"
    "- What remains to be done (clear next steps)\n"
    "- Any critical data, examples, or references needed to continue\n\n"
    "Be concise, structured, and focused on helping the next LLM seamlessly continue the work.";

const std::string kSummaryPrefix =
    "Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:";

} // namespace

const std::string& get_compact_prompt() {
    return kCompactPrompt;
}

const std::string& get_compact_summary_prefix() {
    return kSummaryPrefix;
}

std::string get_compact_user_summary_message(const std::string& summary_text) {
    return kSummaryPrefix + "\n" + summary_text;
}

} // namespace acecode
