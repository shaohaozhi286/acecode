// web/src/lib/errors.js
// 后端 SavedModelEditError 与 HTTP 共用错误码 → 中文文案。
// 未识别码退到原始 message。

const TABLE = {
  INVALID_NAME:      '名字不能为空',
  RESERVED_NAME:     '名字以 ( 开头是系统保留',
  NAME_TAKEN:        '已存在同名条目',
  UNKNOWN_PROVIDER:  '不支持的 provider 类型',
  MISSING_MODEL:     '请填写 model',
  MISSING_BASE_URL:  '该 provider 必须填写 base_url',
  INVALID_API_KEY:   '该 provider 的 API key 不能为空',
  INVALID_CONTEXT_WINDOW: '上下文窗口必须是大于 0 的数字',
  INVALID_CAPABILITY: '模型能力标签无效',
  INVALID_REQUEST_HEADER: '自定义请求头 JSON 无效',
  NOT_FOUND:         '该模型已不存在,请刷新',
  IN_USE_AS_DEFAULT: '该模型正在使用中,稍后再试',
  MODEL_IN_USE:      '该模型正在被运行中的会话使用,稍后再试',
  PERSIST_FAILED:    '配置写盘失败',
  BAD_JSON:          '请求格式错误',
  BAD_REQUEST:       '请求参数错误',
  DEVICE_CODE_FAILED: '无法获取 GitHub 验证码',
  COPILOT_AUTH_REQUIRED: '请先登录 Copilot',
  COPILOT_TOKEN_EXCHANGE_FAILED: 'Copilot 授权校验失败',
  COPILOT_MODELS_UNREACHABLE: '无法连接 Copilot 模型服务',
  COPILOT_MODELS_HTTP_ERROR: 'Copilot 模型服务返回错误',
  COPILOT_MODELS_BAD_JSON: 'Copilot 模型列表格式错误',
  DELETE_FAILED:     '删除凭据失败',
};

export function lookupErrorMessage(code, fallback) {
  if (code && Object.prototype.hasOwnProperty.call(TABLE, code)) return TABLE[code];
  return fallback || code || '未知错误';
}

export const ERROR_CODES = Object.keys(TABLE);
