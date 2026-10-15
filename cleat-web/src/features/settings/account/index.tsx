import { ContentSection } from '../components/content-section'
import { AccountForm } from './account-form'

export function SettingsAccount() {
  return (
    <ContentSection title='账户' desc='当前操作员信息与密码管理。'>
      <AccountForm />
    </ContentSection>
  )
}
