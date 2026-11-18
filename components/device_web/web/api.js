// HTTP 请求及鉴权适配。
async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: {Authorization: `Bearer ${token}`, ...options.headers},
    cache: 'no-store'
  });
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
  return body;
}
