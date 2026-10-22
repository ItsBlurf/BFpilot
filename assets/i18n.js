/* UI strings only. Paths, names, credentials, protocol values and log contents
 * are never translated. No subtree observer rewrites application data. */
(() => {
  'use strict';
  const zh = Object.freeze({
    'Extract archives':'解压归档', 'Extraction results':'解压结果', 'Results':'结果',
    'Destination mode':'目标方式', 'Separate folder for each archive':'为每个归档建立单独文件夹',
    'Current folder':'当前文件夹', 'One custom folder':'同一自定义文件夹', 'Destination folder':'目标文件夹',
    'Merge existing folders and replace matching files':'合并现有文件夹并替换同名文件',
    'Unrelated files stay. Type conflicts stop before publication. Cancellation during merging keeps completed replacements.':'保留无关文件。发布前遇到文件类型冲突会停止。合并期间取消会保留已完成的替换。',
    'Clean partial decoded output after failure':'失败后清理解码产生的部分内容',
    'Partial merge receipts are retained for inspection. A failed archive does not stop later archives; cancellation does.':'保留部分合并的恢复记录供检查。单个归档失败后会继续后续归档；取消则停止后续归档。',
    'Start extraction':'开始解压', 'Cancel extraction':'取消解压',
    'Completed':'已完成', 'Failed':'失败', 'In progress':'进行中', 'Not attempted':'尚未执行',
    'Cancelled':'已取消', '{done} completed, {failed} failed, {pending} not attempted':'已完成 {done}，失败 {failed}，尚未执行 {pending}',
    'Recovery stopped; inspect retained staging and logs before retrying.':'恢复已停止；重试前请检查保留的暂存内容和日志。',
    'Archive queue changed; review current results.':'归档队列已更改；请查看当前结果。',
    'Archive {index} of {count}':'归档 {index} / {count}',
    'Choose a local destination under /data or a USB/ext mount.':'请选择 /data 或 USB/ext 挂载点下的本地目标。',
    'Matching destinations require merge and replace.':'相同目标需要启用合并和替换。',
    'Starting extraction...':'正在启动解压…', 'Select up to 2048 supported archive files':'请选择最多 2048 个受支持的归档文件',
    'Copy archives to local storage before extracting.':'请先将归档复制到本地存储再解压。',
    '{count} archives selected':'已选择 {count} 个归档',
    'Language':'语言', 'File Manager':'文件管理器', 'Connecting':'连接中',
    '{kind} conflict: "{name}" already exists in the target directory. {action}?':'{kind}冲突：目标目录中已存在“{name}”。是否{action}？',
    'Network':'网络', 'USB Drive ':'USB 存储 ', 'Game / App':'游戏 / 应用',
    'Logs':'日志', 'View System Logs':'查看系统日志', 'Exit':'退出', 'Terminate Payload':'退出程序',
    'Places':'位置', 'Files':'文件', 'Root':'根目录', 'Homebrew':'自制程序', 'Mounts':'挂载点', 'User':'用户', 'Data':'数据',
    'Add Shortcut':'添加快捷方式', 'Remove Shortcut':'移除快捷方式', 'Customize Shortcut':'编辑快捷方式',
    'SMB Connections':'SMB 连接', 'Up':'上级', 'Go':'前往', 'Refresh':'刷新', 'Search':'搜索',
    'Search indexed paths':'搜索已索引的路径', 'Index All':'索引全部', 'Cancel':'取消', 'Clear':'清除',
    'Index not built':'尚未建立索引', 'New Folder':'新建文件夹', 'New Text File':'新建文本文件',
    'Upload Files':'上传文件', 'Upload Folder':'上传文件夹', 'No selection':'未选择项目',
    'Open':'打开', 'Open the selected folder':'打开所选文件夹', 'Rename':'重命名', 'Download':'下载',
    'Download mode':'下载方式', 'Combined TAR':'合并为 TAR', 'Individual items':'逐项下载',
    'Individual items may require your browser to allow multiple downloads. Folders download as TAR.':'逐项下载可能需要浏览器允许多个下载。文件夹将以 TAR 格式下载。',
    'Edit':'编辑', 'View':'查看', 'Load payload':'加载载荷',
    'Send selected .elf/.bin/.js to elfldr on port 9021':'将所选 .elf/.bin/.js 发送到端口 9021 的 elfldr',
    'PKG Info':'PKG 信息', 'View PKG metadata and icon':'查看 PKG 元数据和图标',
    'Install PKG':'安装 PKG', 'Install selected PS4/PS5 PKG files':'安装所选 PS4/PS5 PKG 文件',
    'Extract':'解压', 'Delete':'删除', 'More':'更多', 'Change Permissions':'修改权限',
    'Set permissions on selected paths':'设置所选路径的权限',
    'Drop files or folders here to upload into the current folder':'将文件或文件夹拖到此处，上传至当前文件夹',
    'Kind':'种类', 'Name':'名称', 'Type':'类型', 'Size':'大小', 'Modified':'修改时间', 'Permissions':'权限',
    'Transfer':'传输', 'Transfer target and file clipboard':'传输目标和文件剪贴板',
    'Direct destination':'直接传输目标', 'Direct transfer target':'直接传输目标',
    'Select items, choose a destination, then copy or move directly.':'选择项目和目标位置后，即可直接复制或移动。',
    'Use Current':'使用当前位置', 'Open Target':'打开目标', 'Copy to Target':'复制到目标', 'Move to Target':'移动到目标',
    'Clipboard workflow':'剪贴板操作', 'Clipboard empty':'剪贴板为空', 'Select items in the file list.':'请在文件列表中选择项目。',
    'Copy':'复制', 'Move':'移动', 'Paste Here':'粘贴到此处', 'Ready':'就绪', 'Working':'处理中',
    'File download':'文件下载', 'Saved connection':'已保存的连接', 'New connection':'新建连接',
    'Server':'服务器', 'Username':'用户名', 'Domain':'域', 'Password':'密码',
    'hostname or IP, optional :port':'主机名或 IP，可附加 :端口',
    'Keep saved password when editing':'编辑时保留已保存的密码', 'Protocol':'协议',
    'SMB2 / SMB3 automatic':'自动选择 SMB2 / SMB3', 'Require SMB3 encryption':'要求 SMB3 加密',
    'Allow unsigned guest / older NAS connections':'允许未签名的访客或旧 NAS 连接',
    'Saved credentials stay on the PS5. Connections require signing or SMB3 encryption. Choose a saved connection to browse its shared folders.':'凭据仅保存在 PS5 上。默认要求签名或 SMB3 加密。选择已保存的连接以浏览共享文件夹。',
    'Save and Browse':'保存并浏览', 'Remove':'移除', 'Diagnostics & Logs':'诊断和日志', 'Filter logs...':'筛选日志…',
    'Live Real-Time Logs (In-Memory Buffer)':'实时日志（内存缓冲区）',
    'BFpilot Disk Log (/data/BFpilot/log.txt)':'BFpilot 文件日志（/data/BFpilot/log.txt）',
    'PS5 Error History (/system_data/priv/error/history/)':'PS5 错误历史（/system_data/priv/error/history/）',
    'PS5 VSH Log (/system_data/priv/vshlog/vshlog.0.txt)':'PS5 VSH 日志（/system_data/priv/vshlog/vshlog.0.txt）',
    'Search Crawler Log (search_crawl.log)':'搜索索引日志（search_crawl.log）',
    'BFpilot Boot Log (boot.log)':'BFpilot 启动日志（boot.log）',
    'Archive Extractor Log (archive-worker.log)':'解压日志（archive-worker.log）',
    'PKG Installer Log (pkg-installer.log)':'PKG 安装日志（pkg-installer.log）',
    'Close':'关闭', 'Edit File':'编辑文件', 'Save':'保存', 'View Image':'查看图片', 'Reset':'重置',
    'Zoom In':'放大', 'Zoom Out':'缩小', 'Rotate ↻':'旋转 ↻', 'Package Information':'安装包信息',
    'Loading...':'加载中…', 'Title ID':'标题 ID', 'App Version':'应用版本', 'Min Firmware':'最低固件版本',
    'Content ID':'内容 ID', 'Show All Package Headers / SFO Fields':'显示全部安装包头和 SFO 字段',
    'Download PKG':'下载 PKG', 'Install to PS5':'安装到 PS5', 'File Permissions (chmod)':'文件权限（chmod）',
    'Scope':'范围', 'Read (4)':'读取 (4)', 'Write (2)':'写入 (2)', 'Exec (1)':'执行 (1)',
    'Owner':'所有者', 'Group':'所属组', 'Others':'其他用户', 'Octal Mode:':'八进制权限：',
    'Owner read':'所有者读取', 'Owner write':'所有者写入', 'Owner execute':'所有者执行',
    'Group read':'所属组读取', 'Group write':'所属组写入', 'Group execute':'所属组执行',
    'Others read':'其他用户读取', 'Others write':'其他用户写入', 'Others execute':'其他用户执行',
    '0777 Full':'0777 全部', '0755 Exec':'0755 执行', '0644 File':'0644 文件', '0444 R/O':'0444 只读',
    'Apply recursively to all subdirectories & files':'递归应用到所有子目录和文件', 'Apply Permissions':'应用权限',
    'Open / Enter (Cross)':'打开 / 进入（叉键）', 'Select':'选择',
    'Toggle Selection (Square / Space)':'切换选择（方块键 / 空格键）', 'Focus Search (Triangle)':'定位搜索框（三角键）',
    'Storage: loading':'存储：加载中', 'Storage: waiting for data':'存储：等待数据', 'Go to ':'前往 ',
    'Unknown':'未知', ' bytes)':' 字节)', ' usable free of ':' 可用，总计 ', '% used':'% 已用', ' reserved':' 已保留',
    'Shortcut path':'快捷方式路径', 'Shortcut label':'快捷方式名称',
    'Shortcut saved':'快捷方式已保存', 'Open a custom shortcut first':'请先打开自定义快捷方式',
    'Remove shortcut "':'移除快捷方式“', 'Shortcut removed':'快捷方式已移除', 'Shortcut name':'快捷方式名称',
    'Shortcut updated':'快捷方式已更新', ' copied':' 已复制', ' files completed; discovering remaining items':' 个文件已完成；正在查找其余项目',
    'Preparing':'准备中', ' items':' 个项目', ' item':' 个项目', ' (avg ':'（平均 ', 'ETA ':'剩余时间 ',
    ' failed':' 失败', 'Done':'完成', ' files':' 个文件', 'Job prepared':'任务已准备',
    'starting integrated extractor':'正在启动解压器', ' processed by decoder':' 已解码', ' output':' 已输出',
    'write ':'写入 ', 'Waiting for archive status':'等待解压状态', 'Waiting for archive status...':'等待解压状态…',
    'Archive extracted':'解压完成', 'Size so far':'已统计大小', 'Files found':'已找到的文件', 'Path':'路径',
    'Calculating folder size in the background. You can keep browsing.':'正在后台统计文件夹大小，您可以继续浏览。',
    'Folder':'文件夹', 'File':'文件', 'DIR':'目录', 'FILE':'文件', 'Folders':'文件夹', 'SMB server ACL':'SMB 服务器访问权限',
    'Full remote folder size calculated.':'已统计远程文件夹的总大小。',
    'Full folder size calculated without following symbolic links or crossing mounts.':'已统计文件夹总大小，不跟随符号链接或跨越挂载点。',
    'Could not calculate folder size: ':'无法统计文件夹大小：',
    'Move ':'移动 ', 'Copy ':'复制 ', ' more':' 更多', 'Select files or folders first':'请先选择文件或文件夹',
    ' ready. Open a destination and choose Paste Here.':' 已准备。打开目标位置后选择“粘贴到此处”。',
    ' selected':' 项已选', 'Install ':'安装 ', ' PKGs':' 个 PKG', 'Search results':'搜索结果',
    'Items':'项目', ' visible':' 项可见', 'Select a file or folder to see size and modified date.':'选择文件或文件夹以查看大小和修改时间。',
    'Open this connection to browse its shared folders.':'打开此连接以浏览共享文件夹。',
    'Calculating full folder size...':'正在统计文件夹总大小…', 'File size':'文件大小', 'Location':'位置',
    'Folder contents are not included in the total.':'总计不包含文件夹中的内容。', 'SMB ACL':'SMB 权限',
    'Click to edit permissions':'点击修改权限', 'Select all visible items':'选择所有可见项目', 'Loading':'加载中',
    'Indexing system':'正在索引系统', 'Indexing detected mounts':'正在索引已发现的挂载点', ' seen':' 项已发现',
    'roots ':'根目录 ', 'Index not built — press Index All':'尚未建立索引，请选择“索引全部”',
    ' indexed':' 项已索引', ' root fails':' 个根目录失败', 'query ':'查询 ', 'Search status unavailable':'无法获取搜索状态',
    'Search indexing all storage':'正在索引所有存储', ' matches | ':' 个匹配 | ', ' | stale':' | 已失效',
    'rebuild first':'请先重新建立索引', 'Index empty —':'索引为空 —', 'Index Storage Now':'立即索引存储',
    'Finish or cancel the active task first':'请先完成或取消当前任务', 'destination already exists':'目标已存在',
    'Merge':'合并', 'Overwrite':'覆盖', 'Target must be an absolute path':'目标必须为绝对路径',
    'Cannot transfer a folder into itself':'无法将文件夹传输到其自身内部', 'Moved':'已移动', 'Copied':'已复制',
    ' to ':' 到 ', 'Some selected items were not transferred':'部分所选项目未能传输', 'Clipboard is empty':'剪贴板为空',
    'Cannot paste a folder into itself':'无法将文件夹粘贴到其自身内部', 'Some clipboard items were not pasted':'部分剪贴板项目未能粘贴',
    'Download status changed before completion; queue stopped':'下载完成前状态发生变化，队列已停止',
    'Folder name':'文件夹名称', 'Folder created':'文件夹已创建', 'Text file name':'文本文件名称',
    'create text file':'创建文本文件', 'Text file created':'文本文件已创建', 'Select one item':'请选择一个项目',
    'New name':'新名称', 'Renamed':'已重命名', 'Select files or folders':'请选择文件或文件夹',
    '. Allow multiple downloads in your browser and retry.':'。请在浏览器中允许多个下载后重试。',
    'Download queue cancelled':'下载队列已取消', ' downloads sent to browser':' 个下载已发送到浏览器',
    'Download sent to browser':'下载已发送到浏览器', 'PKG installation':'PKG 安装', ' submitted':' 项已提交',
    'install PKG':'安装 PKG', 'PKG install: ':'PKG 安装：',
    'PKG queue accepted by AppInst; watch PS5 notifications':'AppInst 已接受 PKG 队列，请查看 PS5 通知',
    'Select one or more .pkg files':'请选择一个或多个 .pkg 文件', ' package(s)?\n\n':' 个安装包？\n\n',
    'PKG installer started':'PKG 安装器已启动', 'Select one archive file':'请选择一个压缩文件',
    'Select a RAR, 7z, ZIP, TAR or supported compressed archive':'请选择 RAR、7z、ZIP、TAR 或其他受支持的压缩文件',
    'Extract to folder':'解压到文件夹', 'Archive password (leave blank if none)':'压缩包密码（无密码请留空）',
    'Archive extraction started':'解压任务已启动', 'Delete ':'删除 ', ' item(s)?':' 个项目？',
    'network error':'网络错误', 'upload cancelled':'上传已取消', 'Uploaded ':'已上传 ',
    'Upload cancelled after ':'上传已取消，已完成 ', ' file(s)':' 个文件', ' uploaded':' 已上传',
    'Double-click to open folder':'双击打开文件夹', 'Starting':'正在启动', ' downloads | ':' 个下载 | ',
    'Waiting for status...':'等待状态…', 'Lost job status: ':'无法获取任务状态：',
    'Load payload via elfldr (port 9021)?\n\n':'通过 elfldr（端口 9021）加载载荷？\n\n',
    'Sent to elfldr: ':'已发送到 elfldr：', 'Launch failed':'加载失败', 'Launch failed: ':'加载失败：',
    'Select an .elf / .bin / .js payload':'请选择 .elf / .bin / .js 载荷', 'Select a single payload to load':'请选择一个要加载的载荷',
    'Select one folder to open':'请选择一个要打开的文件夹', 'Loading ':'加载中 ', 'Package':'安装包',
    'Loading fields...':'正在加载字段…', 'Invalid package':'无效的安装包', 'Game':'游戏', 'No extra fields':'无其他字段',
    'Could not load package details: ':'无法加载安装包信息：', ' items selected':' 个项目已选',
    'Bad mode (use 0755, 0777, 0644, 0444...)':'无效的权限值（请使用 0755、0777、0644、0444 等）',
    'Applying...':'正在应用…', ' ok':' 成功', 'Loading: ':'正在加载：', 'Editing: ':'正在编辑：',
    'Failed to load file: ':'无法加载文件：', 'Select one editable text file under 5MB or an image':'请选择小于 5MB 的可编辑文本文件或图片',
    'Saving...':'正在保存…', 'Saved successfully':'保存成功', 'Discard unsaved changes?':'放弃尚未保存的更改？',
    'Live log buffer is empty.':'实时日志缓冲区为空。', 'Error reading live logs: ':'读取实时日志失败：',
    'No PS5 error logs found.':'未找到 PS5 错误日志。', 'Unknown Time':'未知时间', 'Unknown Code':'未知代码',
    'Failed to parse error history.':'无法解析错误历史。', 'Error loading error history: ':'加载错误历史失败：',
    'No logs found.':'未找到日志。', 'Cleared.':'已清除。', 'Remove this saved SMB connection?':'移除此已保存的 SMB 连接？',
    'Cancelling...':'正在取消…', 'Cancelling archive...':'正在取消解压…', 'Cancelling upload...':'正在取消上传…',
    'Folder picking is not available in this browser':'此浏览器不支持选择文件夹',
    'Folder picker returned no files; try the drop area':'所选文件夹未返回文件，请尝试拖放上传',
    'Dropped item contained no readable files':'拖放的项目中没有可读取的文件', 'Offline':'离线',
    ' / port 5905':' / 端口 5905', 'Exiting...':'正在退出…',
    'Terminate BFpilot now?\n\nThis will safely exit the payload and return to the PS5 home screen.':'现在退出 BFpilot？\n\n程序将安全退出并返回 PS5 主屏幕。',
    'BFpilot terminated. Returning to home screen...':'BFpilot 已退出，正在返回主屏幕…',
    'ShadowMountPlus scans this destination. Writing directly into a scanned ':'ShadowMountPlus 会扫描此目标。直接写入被扫描的',
    'location while ShadowMountPlus is running can cause a kernel panic.\n\n':'位置可能在 ShadowMountPlus 运行时导致内核崩溃。\n\n',
    'BFpilot stages replacements before finalizing, but the safest option is ':'BFpilot 会先暂存替换内容，但最安全的做法是',
    'to stop ShadowMountPlus or use an unscanned staging folder on the same disk.\n\n':'停止 ShadowMountPlus，或使用同一磁盘上未被扫描的暂存文件夹。\n\n',
    'Continue with ':'继续执行 ',
    'AppInst accepted this package':'AppInst 已接受此安装包',
    'validating package queue':'正在验证安装包队列',
    'waiting for direct AppInst submission':'等待提交到 AppInst',
    'AppInst accepted the queue; monitor PS5 notifications':'AppInst 已接受队列，请查看 PS5 通知',
    'package submission failed or is unconfirmed; check PS5 notifications before retrying':'安装包提交失败或结果尚未确认；重试前请查看 PS5 通知',
    'package submissions failed or are unconfirmed; check PS5 notifications before retrying':'安装包提交失败或结果尚未确认；重试前请查看 PS5 通知',
    'some packages were accepted; others failed or are unconfirmed; check PS5 notifications before retrying':'部分安装包已被接受，其他提交失败或结果尚未确认；重试前请查看 PS5 通知'
  });
  const key = 'bfpilot:language:v1';
  const valid = value => value === 'en' || value === 'zh-CN' || value === 'auto';
  let preference = 'auto';
  try { const saved = localStorage.getItem(key); if (valid(saved)) preference = saved; } catch (_) {}
  const requested = new URL(location.href).searchParams.get('lang');
  if (valid(requested)) preference = requested;
  const browser = (navigator.languages && navigator.languages[0]) || navigator.language || 'en';
  const locale = preference === 'auto' ? (/^zh\b/i.test(browser) ? 'zh-CN' : 'en') : preference;
  const t = (value, parameters) => {
    const translated = locale === 'zh-CN' && Object.prototype.hasOwnProperty.call(zh, value) ? zh[value] : value;
    if (!parameters || typeof translated !== 'string') return translated;
    return translated.replace(/\{([a-zA-Z][a-zA-Z0-9_]*)\}/g, (match, key) =>
      Object.prototype.hasOwnProperty.call(parameters, key) ? String(parameters[key]) : match);
  };
  function translateStatic(root) {
    if (locale !== 'zh-CN') return;
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    let node;
    while ((node = walker.nextNode())) {
      if (!node.parentElement || node.parentElement.closest('script,style,svg,pre,code,textarea,[data-native-language]')) continue;
      const original = node.nodeValue;
      const normalized = original.trim().replace(/\s+/g, ' ');
      if (Object.prototype.hasOwnProperty.call(zh, normalized))
        node.nodeValue = original.match(/^\s*/)[0] + t(normalized) + original.match(/\s*$/)[0];
    }
    const elements = root.querySelectorAll('[title],[aria-label],[placeholder]');
    for (const element of elements) for (const attribute of ['title','aria-label','placeholder']) {
      const original = element.getAttribute(attribute);
      if (original !== null) element.setAttribute(attribute, t(original));
    }
  }
  function staticHTML(markup) {
    const container = document.createElement('div');
    container.innerHTML = markup;
    translateStatic(container);
    return container.innerHTML;
  }
  function setPreference(value) {
    if (!valid(value)) return;
    try { localStorage.setItem(key, value); } catch (_) {}
    const next = new URL(location.href);
    next.searchParams.set('lang', value);
    location.assign(next.href);
  }
  window.BFpilotI18n = Object.freeze({ t, locale, preference, translateStatic, staticHTML, setPreference });
  document.documentElement.lang = locale;
  translateStatic(document.body);
})();
