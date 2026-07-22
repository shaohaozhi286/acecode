// Static text that is intentionally product data rather than presentation.
// These loop template prompts are copied into a user's scheduled task and sent
// to the model verbatim, so changing the display locale must not rewrite them.
export const OPAQUE_STATIC_COPY = Object.freeze([
  '检查工作区最近的代码改动，找出潜在缺陷、兼容性风险和缺失测试。修复明确且安全的问题，运行相关测试，并总结结果。',
  '运行项目的核心测试，调查失败或不稳定用例。修复能够可靠复现且属于项目代码的问题，并报告测试覆盖与遗留风险。',
  '汇总本周工作区中的重要代码变化、已解决问题、仍未完成的事项和技术风险。输出简洁的开发周报与下一步建议，不修改代码。',
  '检查项目依赖的可用更新、已知安全风险和范围明确的技术债。只实施低风险修复，验证构建和测试，并说明其余需要人工决策的事项。',
]);
