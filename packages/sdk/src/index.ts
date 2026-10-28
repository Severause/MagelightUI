export { host, isMagelight } from './host.js';
export { bridge, channel, ready, parsePayload, ensureCore } from './bridge.js';
export { installMock } from './mock.js';
export type {
  HostInfo,
  PageCore,
  Handler,
  Unsubscribe,
  HexColor,
  HostTheme,
  HostThemeResult,
  KeyboardThemeColors,
  CursorThemeColors,
  GamepadControl,
  GamepadAction,
  GamepadState,
  GamepadEvent,
  GamepadScope,
  GamepadHelper,
} from './types.js';
export type { MockOptions } from './mock.js';
