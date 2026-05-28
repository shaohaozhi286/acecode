---
name: vision-image-reader
description: Use a saved model tagged with the vision capability to inspect images when the active model cannot reliably read image content.
---

# Vision Image Reader

当任务需要理解图片内容,而当前模型没有可靠视觉能力时,使用这个 skill。

## Workflow

1. 判断是否真的需要图片内容。只在需要读取截图、照片、界面图、错误弹窗、图表、扫描件等视觉信息时使用。
2. 调用 `vision_analyze` 工具。
3. `prompt` 写清楚要视觉模型回答什么。保持通用,不要把业务流程写死在 skill 里。
4. 如果用户本轮或最近一轮只上传了一张图,可以省略 `attachment_id`,工具会使用当前会话里最新的图片附件。
5. 如果要指定某张图,传 `attachment_id`。
6. 如果要指定某个视觉模型,传 `model_name`;该保存模型必须带 `vision` 能力标签。未指定时工具会从带 `vision` 标签的保存模型里选择一个。
7. 使用工具返回的图片理解结果继续主任务。不要把 `vision_analyze` 的内部调用当作可 resume 或可在界面里切换的会话。

## Tool Shape

```json
{
  "prompt": "Describe the screenshot and identify the visible error.",
  "attachment_id": "optional-image-attachment-id",
  "model_name": "optional-saved-model-name"
}
```

## Guidance

- 如果 `vision_analyze` 返回没有可用视觉模型,提示用户到模型设置里给一个保存模型勾选 `视觉` 能力。
- 如果返回没有图片附件,请用户上传图片,或在下一次调用中提供明确的 `attachment_id`。
- 对多图任务,分别调用工具读取每张图,再在主会话中综合结果。
