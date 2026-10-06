import { createPinia } from 'pinia'
import { createApp } from 'vue'

import App from './App.vue'
import router from './router'
import './styles/main.css'

const app = createApp(App)

// 顺序有讲究：pinia 必须先于 router 安装，因为全局路由守卫会用到 session store。
app.use(createPinia())
app.use(router)

app.mount('#app')
