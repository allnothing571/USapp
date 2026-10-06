import { createPinia } from 'pinia';
import { createApp } from 'vue';

import App from './App.vue';
import { router } from './router';

import './styles/main.css';

const app = createApp(App);

// 注意顺序：先装 pinia，再装 router。
// 路由守卫会在首次导航时读取 pinia store（引导管理会话 / 取 CSRF 令牌），
// 若 pinia 尚未安装，守卫里的 useStore() 会直接抛错。
app.use(createPinia());
app.use(router);

app.mount('#app');
