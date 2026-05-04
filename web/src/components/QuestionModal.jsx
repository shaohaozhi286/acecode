// 兼容旧 import 名称。实际渲染已改为输入框上方的内联 QuestionPicker,
// 不再使用全屏遮罩 modal。

import { QuestionPicker } from './QuestionPicker.jsx';

export function QuestionModal(props) {
  return <QuestionPicker {...props} />;
}
